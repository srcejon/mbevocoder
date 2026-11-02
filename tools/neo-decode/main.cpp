// SPDX-License-Identifier: GPL-3.0-or-later
//
// neo-decode: decode AMBE-3000R channel frames (.dvsi) with mbelib-neo under a set of
// candidate bit mappings, report FEC error statistics per mapping, and write the audio
// for the chosen (or best) mapping. Used for the phase-0 bit-order investigation and,
// later, for decoder-parity tests.
//
//   neo-decode --rate dstar|ambe2450 [--map NAME|all] in.dvsi [out.wav]
//   neo-decode --rate dstar|ambe2450 --dump out.neo in.dvsi
//   neo-decode --synth <prefix>      synthetic parity cases (tests/parity_synth.hpp)
//     Writes one line per frame "nflip c0_errors total_errors <49 bits>" after flipping
//     nflip (0..5, cycling) on-air bits with the same deterministic generator as
//     tests/test_util.hpp, for mbelib-neo parity tests of the bit layer.
//
// mbelib's frame layout: ambe_fr[0][23..0] = C0 (Golay 24,12; index 23 first),
// ambe_fr[1][22..0] = C1 (Golay 23,12), ambe_fr[2][10..0] = C2, ambe_fr[3][13..0] = C3.

#include <mbelib-neo/mbelib.h>

#include "../ambe3000/wav.hpp"
#include "../../tests/parity_synth.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

// dsdcc D-STAR on-air bit -> ambe_fr[dW][dX] (dstar.cpp, GPL-3.0)
const int dW[72] = {
    0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,2,1,1,0,0,3,2, 0,0,3,2,1,1,0,0,1,1,0,0,
    3,2,1,1,3,2,1,1,0,0,3,2, 0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,3,2,1,0,0,3,3 };
const int dX[72] = {
    10,22,11,9,10,22,11,23,8,20,9,21, 10,8,9,21,8,6,7,19,8,20,9,7, 6,18,7,5,6,18,7,19,4,16,5,17,
    6,4,5,17,4,2,3,15,4,16,5,3, 2,14,3,1,2,14,3,15,0,12,1,13, 2,0,1,13,0,12,10,11,0,12,1,13 };

// dsdcc DMR on-air dibit -> ambe_fr[rW][rX] (bit 1), ambe_fr[rY][rZ] (bit 0) (dmr.cpp)
const int rW[36] = { 0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,2, 0,2,0,2,0,2, 0,2,0,2,0,2 };
const int rX[36] = { 23,10,22,9,21,8, 20,7,19,6,18,5, 17,4,16,3,15,2, 14,1,13,0,12,10, 11,9,10,8,9,7, 8,6,7,5,6,4 };
const int rY[36] = { 0,2,0,2,0,2, 0,2,0,3,0,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3 };
const int rZ[36] = { 5,3,4,2,3,1, 2,0,1,13,0,12, 22,11,21,10,20,9, 19,8,18,7,17,6, 16,5,15,4,14,3, 13,2,12,1,11,0 };

using Frame = char[4][24];
using Mapper = std::function<void(const uint8_t* bytes, Frame fr)>;

int bitMsb(const uint8_t* b, int i) { return (b[i / 8] >> (7 - (i % 8))) & 1; }
int bitLsb(const uint8_t* b, int i) { return (b[i / 8] >> (i % 8)) & 1; }

// Sequential C0|C1|C2|C3, first bit into the highest index of each row (mbelib print order).
void seqDesc(const uint8_t* b, Frame fr, int (*bit)(const uint8_t*, int))
{
    std::memset(fr, 0, sizeof(Frame));
    int k = 0;
    for (int j = 23; j >= 0; --j) fr[0][j] = static_cast<char>(bit(b, k++));
    for (int j = 22; j >= 0; --j) fr[1][j] = static_cast<char>(bit(b, k++));
    for (int j = 10; j >= 0; --j) fr[2][j] = static_cast<char>(bit(b, k++));
    for (int j = 13; j >= 0; --j) fr[3][j] = static_cast<char>(bit(b, k++));
}
void seqAsc(const uint8_t* b, Frame fr, int (*bit)(const uint8_t*, int))
{
    std::memset(fr, 0, sizeof(Frame));
    int k = 0;
    for (int j = 0; j < 24; ++j) fr[0][j] = static_cast<char>(bit(b, k++));
    for (int j = 0; j < 23; ++j) fr[1][j] = static_cast<char>(bit(b, k++));
    for (int j = 0; j < 11; ++j) fr[2][j] = static_cast<char>(bit(b, k++));
    for (int j = 0; j < 14; ++j) fr[3][j] = static_cast<char>(bit(b, k++));
}
void dstarMap(const uint8_t* b, Frame fr, int (*bit)(const uint8_t*, int))
{
    std::memset(fr, 0, sizeof(Frame));
    for (int i = 0; i < 72; ++i) fr[dW[i]][dX[i]] = static_cast<char>(bit(b, i));
}
void dmrMap(const uint8_t* b, Frame fr, int (*bit)(const uint8_t*, int))
{
    std::memset(fr, 0, sizeof(Frame));
    for (int d = 0; d < 36; ++d) {
        fr[rW[d]][rX[d]] = static_cast<char>(bit(b, 2 * d));
        fr[rY[d]][rZ[d]] = static_cast<char>(bit(b, 2 * d + 1));
    }
}

struct Candidate { const char* name; Mapper map; };

struct Rng {
    uint32_t x;
    explicit Rng(uint32_t seed) : x(seed * 2654435761u + 12345u) {}
    uint32_t next() { x = x * 1103515245u + 12345u; return (x >> 8) & 0x7fffffu; }
    uint32_t below(uint32_t n) { return next() % n; }
};

void flipBits(char* bits, unsigned nbits, unsigned count, uint32_t frameIndex)
{
    Rng r(frameIndex + 1);
    std::vector<char> done(nbits, 0);
    for (unsigned k = 0; k < count;) {
        uint32_t p = r.below(nbits);
        if (done[p]) continue;
        done[p] = 1;
        bits[p] ^= 1;
        ++k;
    }
}

// Same on-air unpacking as the library: D-STAR LSB first, AMBE+2 MSB first.
// Writes <out>: one text line per frame "nflip c0 total <49 bits> flags status" from the
// frame decode + full process path with injected errors, <out>.noisy.f32 the PCM of that
// path, and <out>.clean.f32 the PCM of the error-free path (float32, 160 per frame).
void dumpParity(const std::vector<uint8_t>& data, bool dstar, const std::string& out)
{
    std::ofstream f(out);
    std::ofstream fNoisy(out + ".noisy.f32", std::ios::binary);
    std::ofstream fClean(out + ".clean.f32", std::ios::binary);
    const size_t n = data.size() / 9;
    mbe_parms cur{}, prev{}, prevEnh{};
    mbe_initMbeParms(&cur, &prev, &prevEnh);
    mbe_parms ccur{}, cprev{}, cprevEnh{};
    mbe_initMbeParms(&ccur, &cprev, &cprevEnh);
    for (size_t i = 0; i < n; ++i) {
        char bits[72], clean[72];
        for (int k = 0; k < 72; ++k) bits[k] = static_cast<char>(dstar ? bitLsb(&data[i * 9], k) : bitMsb(&data[i * 9], k));
        std::memcpy(clean, bits, 72);
        unsigned nflip = static_cast<unsigned>(i % 6);
        flipBits(bits, 72, nflip, static_cast<uint32_t>(i));
        Frame fr, cfr;
        std::memset(fr, 0, sizeof(Frame));
        std::memset(cfr, 0, sizeof(Frame));
        if (dstar) for (int k = 0; k < 72; ++k) { fr[dW[k]][dX[k]] = bits[k]; cfr[dW[k]][dX[k]] = clean[k]; }
        else for (int d = 0; d < 36; ++d) {
            fr[rW[d]][rX[d]] = bits[2 * d]; fr[rY[d]][rZ[d]] = bits[2 * d + 1];
            cfr[rW[d]][rX[d]] = clean[2 * d]; cfr[rY[d]][rZ[d]] = clean[2 * d + 1];
        }
        char d[49];
        mbe_process_result r;
        int total = dstar ? mbe_decodeAmbe3600x2400Frame(fr, d, &r) : mbe_decodeAmbe3600x2450Frame(fr, d, &r);
        f << nflip << ' ' << r.c0_errors << ' ' << total << ' ';
        for (char c : d) f << static_cast<char>('0' + c);
        float pcm[160];
        char d2[49];
        int status = dstar ? mbe_processAmbe3600x2400Framef(pcm, &r, fr, d2, &cur, &prev, &prevEnh)
                           : mbe_processAmbe3600x2450Framef(pcm, &r, fr, d2, &cur, &prev, &prevEnh);
        f << ' ' << r.flags << ' ' << status << '\n';
        fNoisy.write(reinterpret_cast<const char*>(pcm), sizeof pcm);
        status = dstar ? mbe_processAmbe3600x2400Framef(pcm, &r, cfr, d2, &ccur, &cprev, &cprevEnh)
                       : mbe_processAmbe3600x2450Framef(pcm, &r, cfr, d2, &ccur, &cprev, &cprevEnh);
        (void)status;
        fClean.write(reinterpret_cast<const char*>(pcm), sizeof pcm);
    }
}

std::vector<Candidate> candidates()
{
    return {
        {"seq_desc_msb", [](const uint8_t* b, Frame f) { seqDesc(b, f, bitMsb); }},
        {"seq_desc_lsb", [](const uint8_t* b, Frame f) { seqDesc(b, f, bitLsb); }},
        {"seq_asc_msb",  [](const uint8_t* b, Frame f) { seqAsc(b, f, bitMsb); }},
        {"seq_asc_lsb",  [](const uint8_t* b, Frame f) { seqAsc(b, f, bitLsb); }},
        {"dstar_msb",    [](const uint8_t* b, Frame f) { dstarMap(b, f, bitMsb); }},
        {"dstar_lsb",    [](const uint8_t* b, Frame f) { dstarMap(b, f, bitLsb); }},
        {"dmr_msb",      [](const uint8_t* b, Frame f) { dmrMap(b, f, bitMsb); }},
        {"dmr_lsb",      [](const uint8_t* b, Frame f) { dmrMap(b, f, bitLsb); }},
    };
}

struct Stats { long c0 = 0, total = 0, frames = 0, badStatus = 0, tone = 0; };

Stats runMapping(const std::vector<uint8_t>& data, bool dstar, const Mapper& map, std::vector<float>* audio)
{
    mbe_parms cur{}, prev{}, prevEnh{};
    mbe_initMbeParms(&cur, &prev, &prevEnh);
    Stats st;
    const size_t n = data.size() / 9;
    for (size_t i = 0; i < n; ++i) {
        Frame fr;
        map(&data[i * 9], fr);
        char d[49];
        float out[160];
        mbe_process_result r;
        mbe_initProcessResult(&r);
        int status = dstar ? mbe_processAmbe3600x2400Framef(out, &r, fr, d, &cur, &prev, &prevEnh)
                           : mbe_processAmbe3600x2450Framef(out, &r, fr, d, &cur, &prev, &prevEnh);
        if (status < 0) { ++st.badStatus; if (audio) audio->insert(audio->end(), 160, 0.f); continue; }
        st.c0 += r.c0_errors;
        st.total += r.total_errors;
        if (r.flags & MBE_PROCESS_FLAG_TONE) ++st.tone;
        ++st.frames;
        if (audio) audio->insert(audio->end(), out, out + 160);
    }
    return st;
}

} // namespace

int main(int argc, char** argv)
{
    std::string rate, mapName = "all", in, out, dump;
    if (argc == 3 && std::string(argv[1]) == "--synth") { parity::runAll(argv[2]); return 0; }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--map" && i + 1 < argc) mapName = argv[++i];
        else if (in.empty()) in = a;
        else out = a;
    }
    if (in.empty() || (rate != "dstar" && rate != "ambe2450")) {
        std::fprintf(stderr, "usage: neo-decode --rate dstar|ambe2450 [--map NAME|all] in.dvsi [out.wav]\n");
        return 2;
    }
    const bool dstar = rate == "dstar";
    std::ifstream f(in, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() % 9) std::fprintf(stderr, "warning: size not a multiple of 9\n");
    if (!dump.empty()) { dumpParity(data, dstar, dump); return 0; }

    std::printf("mbelib-neo %s, %zu frames, rate %s\n", mbe_versionString(), data.size() / 9, rate.c_str());
    std::printf("%-14s %8s %8s %8s %6s %6s\n", "mapping", "c0err", "totalerr", "err/frm", "tone", "bad");
    const Candidate* best = nullptr;
    double bestErr = 1e9;
    for (const auto& c : candidates()) {
        if (mapName != "all" && mapName != c.name) continue;
        Stats st = runMapping(data, dstar, c.map, nullptr);
        double perFrame = st.frames ? double(st.total) / double(st.frames) : 999;
        std::printf("%-14s %8ld %8ld %8.3f %6ld %6ld\n", c.name, st.c0, st.total, perFrame, st.tone, st.badStatus);
        if (perFrame < bestErr) { bestErr = perFrame; best = &c; }
    }
    if (!best) { std::fprintf(stderr, "unknown mapping %s\n", mapName.c_str()); return 1; }
    if (!out.empty()) {
        // re-run best (iterators above went out of scope; rebuild by name)
        for (const auto& c : candidates()) {
            if (std::strcmp(c.name, best->name) != 0) continue;
            std::vector<float> audio;
            runMapping(data, dstar, c.map, &audio);
            wav::Audio w; w.sampleRate = 8000;
            w.samples.reserve(audio.size());
            for (float s : audio) {
                float v = s < -32767.f ? -32767.f : (s > 32767.f ? 32767.f : s);
                w.samples.push_back(static_cast<int16_t>(v));
            }
            wav::write(out, w);
            std::printf("wrote %s using mapping %s\n", out.c_str(), c.name);
        }
    }
    return 0;
}
