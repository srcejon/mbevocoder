// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic decoder parity cases. This header is compiled both against mbelib-neo (in
// tools/neo-decode) and against mbevocoder (in tests/test_decoder_parity); the two libraries
// share the mbe_* API, so the same code produces the reference and the candidate output.
// Each case writes <prefix>.<case>.f32 (float PCM, 160 per frame) and <prefix>.<case>.txt
// (one line per frame: "status flags total").
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace parity {

struct Rng {
    uint32_t x;
    explicit Rng(uint32_t seed) : x(seed * 2654435761u + 12345u) {}
    uint32_t next() { x = x * 1103515245u + 12345u; return (x >> 8) & 0x7fffffu; }
    uint32_t below(uint32_t n) { return next() % n; }
    char bit() { return static_cast<char>(next() & 1u); }
};

struct Case {
    const char* name;
    unsigned frames;
};

inline const std::vector<Case>& cases()
{
    static const std::vector<Case> c = {
        {"imbe4400_data", 3000}, {"imbe7200_frame", 3000}, {"imbe7100_frame", 2000},
        {"ambe2400_data", 3000}, {"ambe2450_data", 3000},
        {"ambe2400_softframe", 2000}, {"ambe2450_softframe", 2000},
        {"imbe7200_softframe", 2000}, {"ambe2450_short", 1000},
    };
    return c;
}

// Uniformly random bits; random frames fail FEC often enough to exercise the erasure,
// repeat, mute and comfort-noise paths.
inline void randomBits(Rng& r, char* bits, int n)
{
    for (int i = 0; i < n; ++i) bits[i] = r.bit();
}

inline void runCase(const Case& c, const std::string& prefix)
{
    std::ofstream pcmOut(prefix + "." + c.name + ".f32", std::ios::binary);
    std::ofstream txt(prefix + "." + c.name + ".txt");
    // AMBE+2 voicing row per frame (-1 for other rates): the parity test exempts the rows
    // mbevocoder renders differently from mbelib-neo (MBEVOCODER_CHIP_MEASURED, rows 1, 13 and 15).
    std::ofstream rows(prefix + "." + c.name + ".vuvrow");
    std::ofstream c0err(prefix + "." + c.name + ".c0err");   // c0_errors per frame (4 = detected uncorrectable)
    Rng r(static_cast<uint32_t>(std::strlen(c.name)) * 7919u + c.frames);
    mbe_parms cur, prev, prevEnh;
    mbe_initMbeParms(&cur, &prev, &prevEnh);
    const std::string name = c.name;
    for (unsigned i = 0; i < c.frames; ++i) {
        float pcm[160];
        short spcm[160];
        mbe_process_result res;
        mbe_initProcessResult(&res);
        int status = 0;
        int vuvRow = -1;
        char d49[49] = {0};
        if (name == "imbe4400_data") {
            char d[88]; randomBits(r, d, 88);
            status = mbe_processImbe4400Dataf(pcm, &res, d, &cur, &prev, &prevEnh);
        } else if (name == "imbe7200_frame") {
            char fr[8][23], d[88]; randomBits(r, &fr[0][0], 8 * 23);
            status = mbe_processImbe7200x4400Framef(pcm, &res, fr, d, &cur, &prev, &prevEnh);
        } else if (name == "imbe7100_frame") {
            char fr[7][24], d[88]; randomBits(r, &fr[0][0], 7 * 24);
            status = mbe_processImbe7100x4400Framef(pcm, &res, fr, d, &cur, &prev, &prevEnh);
        } else if (name == "ambe2400_data") {
            char d[49]; randomBits(r, d, 49);
            status = mbe_processAmbe2400Dataf(pcm, &res, d, &cur, &prev, &prevEnh);
        } else if (name == "ambe2450_data") {
            char d[49]; randomBits(r, d, 49);
            status = mbe_processAmbe2450Dataf(pcm, &res, d, &cur, &prev, &prevEnh);
            std::memcpy(d49, d, 49);
        } else if (name == "ambe2400_softframe" || name == "ambe2450_softframe") {
            mbe_soft_bit fr[4][24];
            char d[49];
            for (int k = 0; k < 96; ++k) { (&fr[0][0])[k].bit = static_cast<uint8_t>(r.bit()); (&fr[0][0])[k].reliability = static_cast<uint8_t>(r.below(256)); }
            status = name == "ambe2400_softframe" ? mbe_processAmbe3600x2400SoftFramef(pcm, &res, fr, d, &cur, &prev, &prevEnh)
                                                  : mbe_processAmbe3600x2450SoftFramef(pcm, &res, fr, d, &cur, &prev, &prevEnh);
            if (name == "ambe2450_softframe") std::memcpy(d49, d, 49);
        } else if (name == "imbe7200_softframe") {
            mbe_soft_bit fr[8][23];
            char d[88];
            for (int k = 0; k < 8 * 23; ++k) { (&fr[0][0])[k].bit = static_cast<uint8_t>(r.bit()); (&fr[0][0])[k].reliability = static_cast<uint8_t>(r.below(256)); }
            status = mbe_processImbe7200x4400SoftFramef(pcm, &res, fr, d, &cur, &prev, &prevEnh);
        } else if (name == "ambe2450_short") {
            char fr[4][24], d[49]; randomBits(r, &fr[0][0], 96);
            status = mbe_processAmbe3600x2450Frame(spcm, &res, fr, d, &cur, &prev, &prevEnh);
            for (int k = 0; k < 160; ++k) pcm[k] = static_cast<float>(spcm[k]);
            std::memcpy(d49, d, 49);
        }
        if (status < 0) std::memset(pcm, 0, sizeof pcm);
        if (name.find("2450") != std::string::npos)
            vuvRow = (d49[4] << 4) | (d49[5] << 3) | (d49[6] << 2) | (d49[7] << 1) | d49[35];
        pcmOut.write(reinterpret_cast<const char*>(pcm), sizeof pcm);
        txt << status << ' ' << res.flags << ' ' << res.total_errors << '\n';
        rows << vuvRow << '\n';
        c0err << res.c0_errors << '\n';
    }
}

// One thread per case so thread-local decoder state (comfort-noise RNG, FFT plan) starts
// fresh for every case on both sides of the comparison.
inline void runAll(const std::string& prefix)
{
    for (const Case& c : cases()) {
        std::thread t([&] { runCase(c, prefix); });
        t.join();
    }
}

} // namespace parity
