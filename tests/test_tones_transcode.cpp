// SPDX-License-Identifier: GPL-3.0-or-later
// Tone frames and the parameter-domain transcoder, checked against AMBE-3000R captures
// (tests/data/tones: the AMBE-3000R's encoder output for known tones on both AMBE rates).
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;
std::string g_dataDir;

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// AMBE-3000R capture -> parameter bits per frame (FEC decoded).
std::vector<std::vector<char>> captureData(const std::string& name, bool dstar)
{
    auto bytes = readFile(g_dataDir + "/" + name + (dstar ? ".dstar.dvsi" : ".ambe2450.dvsi"));
    std::vector<std::vector<char>> out;
    for (size_t i = 0; i + 9 <= bytes.size(); i += 9) {
        char bits[72], fr[4][24];
        std::vector<char> d(49);
        mbe_unpackBits(&bytes[i], 72, bits, dstar ? 0 : 1);
        mbe_ambeFrameFromBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits, fr);
        mbe_process_result r;
        if (dstar) mbe_decodeAmbe3600x2400Frame(fr, d.data(), &r);
        else mbe_decodeAmbe3600x2450Frame(fr, d.data(), &r);
        out.push_back(d);
    }
    return out;
}

int bitsToInt(const char* d, int first, int count)
{
    int v = 0;
    for (int i = 0; i < count; ++i) v = (v << 1) | d[first + i];
    return v;
}

struct Expect { const char* name; int id; float f1, f2; int knox; };
// Expected AMBE+2 tone ids for the captures (AMBE-3000R-verified numbering = mbelib's table).
const Expect kExpect[] = {
    {"t1000", 32, 1000, 1000, 0}, {"t500", 16, 500, 500, 0}, {"t2000", 64, 2000, 2000, 0}, {"s156", 5, 156.25f, 156.25f, 0},
    {"s187", 6, 187.5f, 187.5f, 0}, {"s218", 7, 218.75f, 218.75f, 0}, {"s3812", 122, 3812.5f, 3812.5f, 0}, {"s1500", 48, 1500, 1500, 0},
    {"d0", 128, 1336, 941, 0}, {"dtmf1", 129, 1209, 697, 0}, {"d2", 130, 1336, 697, 0}, {"d5", 133, 1336, 770, 0},
    {"d9", 137, 1477, 852, 0}, {"dA", 138, 1633, 697, 0}, {"dD", 141, 1633, 941, 0}, {"dstar", 142, 1209, 941, 0},
    {"dhash", 143, 1477, 941, 0}, {"k0", 144, 1162, 820, 1}, {"k15", 159, 1279, 820, 1},
    {"cp_dial", 160, 440, 350, 0}, {"cp_ring", 161, 480, 440, 0}, {"cp_busy", 162, 620, 480, 0}, {"cp_reorder", 163, 490, 350, 0},
};

// Dominant frequencies of a 160-sample float frame sequence (FFT over frames a..b).
void peaks(const std::vector<float>& x, float* f1, float* f2, float* dbfs)
{
    const int n = static_cast<int>(x.size());
    std::vector<float> mag(n / 2 + 1, 0.f);
    double energy = 0;
    for (int i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
    for (int k = 0; k <= n / 2; ++k) {
        double re = 0, im = 0;
        for (int i = 0; i < n; ++i) {
            double w = 0.5 - 0.5 * std::cos(2 * static_cast<double>(kPi) * i / static_cast<double>(n));
            re += w * x[i] * std::cos(2 * kPi * static_cast<double>(k) * i / static_cast<double>(n));
            im -= w * x[i] * std::sin(2 * kPi * static_cast<double>(k) * i / static_cast<double>(n));
        }
        mag[k] = static_cast<float>(std::sqrt(re * re + im * im));
    }
    int b1 = 0;
    for (int k = 1; k <= n / 2; ++k) if (mag[k] > mag[b1]) b1 = k;
    int b2 = -1;
    for (int k = 1; k <= n / 2; ++k) if (std::abs(k - b1) * 8000 / n > 30 && (b2 < 0 || mag[k] > mag[b2])) b2 = k;
    *f1 = static_cast<float>(b1) * 8000.f / static_cast<float>(n);
    *f2 = static_cast<float>(b2) * 8000.f / static_cast<float>(n);
    float rms = static_cast<float>(std::sqrt(energy / n));
    *dbfs = 20.f * std::log10(rms * std::sqrt(2.f) / (32767.f * 0.95f / 7.f) + 1e-9f);   // relative to full scale in the float domain
}

void toneFrames()
{
    // API basics
    float f1, f2;
    CHECK_EQ(mbe_toneFrequencies(32, &f1, &f2), 1); CHECK(f1 == 1000.f && f2 == 1000.f);
    CHECK_EQ(mbe_toneFrequencies(129, &f1, &f2), 1); CHECK(f1 == 1209.f && f2 == 697.f);
    CHECK_EQ(mbe_toneFrequencies(4, &f1, &f2), 0);
    CHECK_EQ(mbe_toneFrequencies(164, &f1, &f2), 0);
    CHECK_EQ(mbe_toneIdFromFrequencies(1000, 0), 32);
    CHECK_EQ(mbe_toneIdFromFrequencies(697, 1209), 129);
    CHECK_EQ(mbe_toneIdFromFrequencies(156, 0), 5);
    CHECK_EQ(mbe_toneIdToDstar(129), 128);   // DTMF 1: column 0, row 0
    CHECK_EQ(mbe_toneIdToDstar(128), 135);   // DTMF 0: column 1, row 3
    CHECK_EQ(mbe_toneIdToDstar(163), 147);
    CHECK_EQ(mbe_toneIdToDstar(144), -1);    // KNOX
    for (int id = 128; id <= 163; ++id) {
        int d = mbe_toneIdToDstar(id);
        if (d >= 0) CHECK_EQ(mbe_toneIdFromDstar(d), id);
    }
    char d[49];
    CHECK_EQ(mbe_encodeAmbe2450ToneData(32, 128, d), MBE_STATUS_INVALID_ARGUMENT);
    CHECK_EQ(mbe_encodeAmbe2450ToneData(200, 100, d), MBE_STATUS_INVALID_ARGUMENT);
    CHECK_EQ(mbe_encodeAmbe2400ToneData(144, 100, d), MBE_STATUS_ENCODE_RANGE);

    // AMBE-3000R captures: frames 11..60 are tone frames; our builder reproduces them.
    int captures = 0;
    for (const Expect& e : kExpect) {
        for (int dstar = 0; dstar < 2; ++dstar) {
            auto frames = captureData(e.name, dstar != 0);
            if (frames.empty()) continue;
            ++captures;
            CHECK_EQ(frames.size(), 74u);
            if (dstar && e.knox) {
                // D-STAR has no KNOX tones: the AMBE-3000R encodes them as voice.
                for (int i = 11; i <= 60; ++i) CHECK(bitsToInt(frames[i].data(), 0, 6) != 63);
                continue;
            }
            int toneFrames = 0, reproduced = 0;
            for (int i = 0; i < 74; ++i) {
                const char* c = frames[i].data();
                char ours[49];
                if (!dstar) {
                    if (bitsToInt(c, 0, 6) != 63) continue;
                    ++toneFrames;
                    int id = bitsToInt(c, 12, 8);
                    int ad = (bitsToInt(c, 6, 6) << 1) | c[44];
                    if (id != e.id) continue;
                    CHECK_EQ(mbe_encodeAmbe2450ToneData(id, ad, ours), 0);
                    if (std::memcmp(ours, c, 49) == 0) ++reproduced;
                } else {
                    int b0 = (bitsToInt(c, 0, 6) << 1) | c[48];
                    if (b0 != 126 && b0 != 127) continue;
                    ++toneFrames;
                    // The D-STAR volume has one more bit of resolution than the AMBE+2
                    // amplitude: compare the index bits exactly and the volume within 1.
                    int vol = (bitsToInt(c, 12, 5) << 3) | (c[44] << 2) | (c[45] << 1) | c[17];
                    int amp = (vol + 17) / 2;
                    CHECK_EQ(mbe_encodeAmbe2400ToneData(e.id, amp > 127 ? 127 : amp, ours), 0);
                    int ourVol = (bitsToInt(ours, 12, 5) << 3) | (ours[44] << 2) | (ours[45] << 1) | ours[17];
                    bool same = std::abs(ourVol - vol) <= 1;
                    for (int k = 0; k < 49; ++k) {
                        bool volBit = (k >= 12 && k <= 17) || k == 44 || k == 45;
                        if (k == 24) continue;   // C1 parity, filled in by the frame encoder
                        if (!volBit && ours[k] != c[k]) same = false;
                    }
                    if (same) ++reproduced;
                }
            }
            CHECK(toneFrames >= 45);
            if (reproduced != toneFrames) std::printf("  %s %s: %d of %d tone frames reproduced\n", e.name, dstar ? "dstar" : "ambe2450", reproduced, toneFrames);
            CHECK_EQ(reproduced, toneFrames);
        }
    }
    CHECK(captures >= 40);
    std::printf("tone captures checked: %d\n", captures);

    // Decoder: our tone frames render the right frequencies, the AMBE-3000R's amplitude law
    // (0.71 dB per AD step) and equal-level dual tones; D-STAR renders dual tones too.
    for (int dstar = 0; dstar < 2; ++dstar) {
        mbe_encoder* enc = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, nullptr);
        mbe_parms cur, prev, enh;
        auto render = [&](int id, int amp, float* pf1, float* pf2, float* pdb) {
            mbe_initMbeParms(&cur, &prev, &enh);
            std::vector<float> out;
            for (int i = 0; i < 8; ++i) {
                char bits[72], fr[4][24], dd[49];
                float pcm[160];
                mbe_process_result r;
                CHECK_EQ(mbe_encoderTone(enc, id, amp, dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits), 0);
                mbe_ambeFrameFromBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits, fr);
                int st = dstar ? mbe_processAmbe3600x2400Framef(pcm, &r, fr, dd, &cur, &prev, &enh)
                               : mbe_processAmbe3600x2450Framef(pcm, &r, fr, dd, &cur, &prev, &enh);
                CHECK(st >= 0);
                CHECK(r.flags & MBE_PROCESS_FLAG_TONE);
                if (i >= 2) out.insert(out.end(), pcm, pcm + 160);
            }
            peaks(out, pf1, pf2, pdb);
        };
        float a1, a2, adb, b1, b2, bdb, c1, c2, cdb;
        render(32, 112, &a1, &a2, &adb);
        render(32, 84, &b1, &b2, &bdb);
        render(129, 112, &c1, &c2, &cdb);
        std::printf("%s tones: 1000 Hz -> %.0f Hz %.1f dB; AD 84 -> %.1f dB; DTMF 1 -> %.0f/%.0f Hz %.1f dB\n",
                    dstar ? "D-STAR" : "AMBE+2", a1, adb, bdb, c1, c2, cdb);
        CHECK(std::fabs(a1 - 1000.f) < 10.f);
#if MBEVOCODER_CHIP_MEASURED
        CHECK(std::fabs(adb - (-10.2f)) < 1.0f);   // the AMBE-3000R's amplitude law (mbelib's differs)
        CHECK(std::fabs((adb - bdb) - 19.9f) < 1.0f);
        CHECK(std::fabs(cdb - adb - 3.0f) < 1.0f);
#endif
        if (!dstar || MBEVOCODER_CHIP_MEASURED)   // mbelib renders D-STAR dual tones as comfort noise
            CHECK((std::fabs(c1 - 697.f) < 12.f && std::fabs(c2 - 1209.f) < 12.f) || (std::fabs(c2 - 697.f) < 12.f && std::fabs(c1 - 1209.f) < 12.f));
        mbe_encoderDestroy(enc);
    }
}

std::vector<short> synthVoice(float f0, float seconds, float amp = 8000.f)
{
    int n = static_cast<int>(seconds * 8000);
    std::vector<short> s(n);
    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / 8000.f;
        float v = 0;
        for (int h = 1; h <= 20 && f0 * static_cast<float>(h) < 3800; ++h) v += std::sin(2 * kPi * f0 * static_cast<float>(h) * t) / static_cast<float>(h);
        v *= 0.6f + 0.4f * std::sin(2 * kPi * 3.f * t);
        s[i] = static_cast<short>(amp * v / 2.0f);
    }
    return s;
}

// Decode a parameter-bit stream with the process functions; returns mean rms and the
// fraction of frames whose pitch is within 6 % of f0.
void decodeStream(mbe_rate rate, const std::vector<std::vector<char>>& frames, float f0, double* rmsOut, double* pitchFrac)
{
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    double e = 0; int ok = 0, n = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        float pcm[160];
        mbe_process_result r;
        mbe_initProcessResult(&r);   // the data-level process functions read the FEC stage's result
        int st;
        if (rate == MBE_RATE_AMBE3600x2400) st = mbe_processAmbe2400Dataf(pcm, &r, frames[i].data(), &cur, &prev, &enh);
        else if (rate == MBE_RATE_IMBE4400 || rate == MBE_RATE_IMBE7200x4400) st = mbe_processImbe4400Dataf(pcm, &r, frames[i].data(), &cur, &prev, &enh);
        else st = mbe_processAmbe2450Dataf(pcm, &r, frames[i].data(), &cur, &prev, &enh);
        if (st < 0) std::printf("decodeStream rate %d frame %zu status %d\n", static_cast<int>(rate), i, st);
        CHECK(st >= 0);
        if (i < 10) continue;
        double s = 0;
        for (float v : pcm) s += static_cast<double>(v) * v;
        e += std::sqrt(s / 160);
        float f0Dec = prev.w0 * 8000.f / (2 * kPi);
        if (std::fabs(f0Dec - f0) < 0.06f * f0) ++ok;
        ++n;
    }
    *rmsOut = n ? e / n : 0;
    *pitchFrac = n ? static_cast<double>(ok) / n : 0;
}

void transcoder()
{
    CHECK(mbe_transcoderCreate(MBE_RATE_COUNT, MBE_RATE_AMBE2450) == nullptr);
    mbe_transcoder* t = mbe_transcoderCreate(MBE_RATE_AMBE2450, MBE_RATE_AMBE3600x2400);
    CHECK(t != nullptr);
    CHECK_EQ(mbe_transcoderSourceRate(t), MBE_RATE_AMBE2450);
    CHECK_EQ(mbe_transcoderTargetRate(t), MBE_RATE_AMBE3600x2400);
    CHECK(mbe_transcoderModel(t) == nullptr);
    char bad[49];
    std::memset(bad, 2, sizeof bad);
    char out[88];
    CHECK_EQ(mbe_transcoderProcess(t, bad, out), MBE_STATUS_INVALID_BITS);
    CHECK_EQ(mbe_transcoderProcess(nullptr, bad, out), MBE_STATUS_INVALID_ARGUMENT);
    mbe_transcoderDestroy(t);
    mbe_transcoderDestroy(nullptr);

    // Speech: AMBE+2 source, every target; the target decodes with the source's pitch and level.
    const float f0 = 150.f;
    auto sig = synthVoice(f0, 1.0f);
    std::vector<std::vector<char>> src;
    {
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE2450, nullptr);
        for (size_t i = 0; i + 160 <= sig.size(); i += 160) {
            std::vector<char> d(49);
            CHECK_EQ(mbe_encoderEncodeParms(e, &sig[i], d.data(), nullptr), 0);
            src.push_back(d);
        }
        mbe_encoderDestroy(e);
    }
    double srcRms, srcPitch;
    decodeStream(MBE_RATE_AMBE2450, src, f0, &srcRms, &srcPitch);
    const mbe_rate targets[] = {MBE_RATE_AMBE3600x2400, MBE_RATE_IMBE7200x4400, MBE_RATE_AMBE3600x2450};
    for (mbe_rate to : targets) {
        t = mbe_transcoderCreate(MBE_RATE_AMBE2450, to);
        std::vector<std::vector<char>> dst;
        for (const auto& d : src) {
            std::vector<char> o(88);
            CHECK_EQ(mbe_transcoderProcess(t, d.data(), o.data()), MBE_TRANSCODE_VOICE);
            dst.push_back(o);
        }
        if (to != MBE_RATE_AMBE3600x2450) CHECK(mbe_transcoderModel(t) != nullptr);   // pass-through decodes nothing
        double rms, pitch;
        decodeStream(to, dst, f0, &rms, &pitch);
        double levelDb = 20 * std::log10(rms / srcRms);
        std::printf("transcode 2450 -> rate %d: level %+.1f dB, pitch ok %.0f%% (source %.0f%%)\n", static_cast<int>(to), levelDb, pitch * 100, srcPitch * 100);
        // The D-STAR target is deliberately 0.3 dB lower (the AMBE-3000R's D-STAR decoder is that
        // much louder than its AMBE+2 decoder, docs/transcoder.md); our decoders are consistent.
        const double expectDb = to == MBE_RATE_AMBE3600x2400 ? -0.3 : 0.0;
        CHECK(std::fabs(levelDb - expectDb) < 2.0);
        CHECK(pitch > 0.8);
        if (to == MBE_RATE_AMBE3600x2450) {   // same format: pass-through
            for (size_t i = 0; i < src.size(); ++i) CHECK_EQ(std::memcmp(src[i].data(), dst[i].data(), 49), 0);
        }
        // And back to AMBE+2 from that target.
        mbe_transcoder* back = mbe_transcoderCreate(to, MBE_RATE_AMBE2450);
        std::vector<std::vector<char>> again;
        for (const auto& d : dst) {
            std::vector<char> o(49);
            CHECK_EQ(mbe_transcoderProcess(back, d.data(), o.data()), MBE_TRANSCODE_VOICE);
            again.push_back(o);
        }
        decodeStream(MBE_RATE_AMBE2450, again, f0, &rms, &pitch);
        levelDb = 20 * std::log10(rms / srcRms);
        std::printf("  and back: level %+.1f dB, pitch ok %.0f%%\n", levelDb, pitch * 100);
        const double expectBackDb = to == MBE_RATE_AMBE3600x2400 ? -0.3 + 0.45 : 0.0;
        CHECK(std::fabs(levelDb - expectBackDb) < 2.5);
        CHECK(pitch > 0.8);
        mbe_transcoderDestroy(back);
        mbe_transcoderDestroy(t);
    }

    // Tones: the AMBE-3000R's D-STAR DTMF capture transcodes to the AMBE-3000R's AMBE+2 capture bit
    // for bit, and back; KNOX tones cannot go to D-STAR (repeat); IMBE gets a repeat.
    auto dstarDtmf = captureData("dtmf1", true), ambeDtmf = captureData("dtmf1", false), knox = captureData("k0", false);
    if (!dstarDtmf.empty() && !ambeDtmf.empty()) {
        t = mbe_transcoderCreate(MBE_RATE_AMBE3600x2400, MBE_RATE_AMBE3600x2450);
        mbe_transcoder* back = mbe_transcoderCreate(MBE_RATE_AMBE3600x2450, MBE_RATE_AMBE3600x2400);
        mbe_transcoder* toImbe = mbe_transcoderCreate(MBE_RATE_AMBE3600x2450, MBE_RATE_IMBE4400);
        int exact = 0, exactBack = 0;
        for (int i = 11; i <= 60; ++i) {
            char o[49], o2[49], o3[88];
            CHECK_EQ(mbe_transcoderProcess(t, dstarDtmf[i].data(), o), MBE_TRANSCODE_TONE);
            if (std::memcmp(o, ambeDtmf[i].data(), 49) == 0) ++exact;
            CHECK_EQ(mbe_transcoderProcess(back, ambeDtmf[i].data(), o2), MBE_TRANSCODE_TONE);
            bool same = true;
            for (int k = 0; k < 49; ++k) if (k != 17 && o2[k] != dstarDtmf[i][k]) same = false;
            if (same) ++exactBack;
            CHECK_EQ(mbe_transcoderProcess(toImbe, ambeDtmf[i].data(), o3), MBE_TRANSCODE_REPEAT);
        }
        CHECK_EQ(exact, 50);
        CHECK_EQ(exactBack, 50);
        if (!knox.empty()) {
            char o[49];
            CHECK_EQ(mbe_transcoderProcess(back, knox[30].data(), o), MBE_TRANSCODE_REPEAT);
        }
        mbe_transcoderDestroy(t);
        mbe_transcoderDestroy(back);
        mbe_transcoderDestroy(toImbe);
    }

    // Same-format pass-through copies the bits but still reports the frame type.
    {
        mbe_transcoder* same = mbe_transcoderCreate(MBE_RATE_AMBE2450, MBE_RATE_AMBE3600x2450);
        CHECK(same != nullptr);
        char tone[49], o[49];
        CHECK_EQ(mbe_encodeAmbe2450ToneData(0x20, 100, tone), 0);
        CHECK_EQ(mbe_transcoderProcess(same, tone, o), MBE_TRANSCODE_TONE);
        CHECK(std::memcmp(o, tone, 49) == 0);
        char voice[49] = {0};
        voice[0] = 1;   // b0 = 64: an ordinary pitch, not a tone pattern
        CHECK_EQ(mbe_transcoderProcess(same, voice, o), MBE_TRANSCODE_VOICE);
        CHECK(std::memcmp(o, voice, 49) == 0);
        // An erasure is not passed through: the previous model is repeated as a usable
        // frame and mbe_transcoderModel reports it, with or without an earlier voice frame.
        char erasure[49] = {1, 1, 1, 1, 0};   // b0 = 120
        mbe_transcoderReset(same);
        CHECK(mbe_transcoderModel(same) == nullptr);
        CHECK_EQ(mbe_transcoderProcess(same, erasure, o), MBE_TRANSCODE_REPEAT);
        CHECK(std::memcmp(o, erasure, 49) != 0);
        CHECK(mbe_transcoderModel(same) != nullptr);
        {
            mbe_parms cur, prev, enh;
            mbe_initMbeParms(&cur, &prev, &enh);
            CHECK_EQ(mbe_decodeAmbe2450Parms(o, &cur, &prev), 0);
        }
        CHECK_EQ(mbe_transcoderProcess(same, voice, o), MBE_TRANSCODE_VOICE);
        const mbe_parms* before = mbe_transcoderModel(same);
        CHECK(before != nullptr);
        const float w0Before = before->w0;
        CHECK_EQ(mbe_transcoderProcess(same, erasure, o), MBE_TRANSCODE_REPEAT);
        CHECK(std::memcmp(o, erasure, 49) != 0);
        CHECK(mbe_transcoderModel(same) != nullptr);
        CHECK(std::fabs(mbe_transcoderModel(same)->w0 - w0Before) < 1e-6f);
        mbe_transcoderDestroy(same);
        // D-STAR to D-STAR is a same-format pair too: voice frames pass through unchanged
        {
            mbe_transcoder* dd = mbe_transcoderCreate(MBE_RATE_AMBE3600x2400, MBE_RATE_AMBE3600x2400);
            CHECK(dd != nullptr);
            int bv[9] = {64, 15, 30, 100, 40, 8, 4, 4, 2};
            char v[49];
            CHECK_EQ(mbe_packAmbe2400Fields(bv, v), 0);
            for (int i = 0; i < 3; ++i) {
                CHECK_EQ(mbe_transcoderProcess(dd, v, o), MBE_TRANSCODE_VOICE);
                CHECK(std::memcmp(o, v, 49) == 0);
            }
            mbe_transcoderDestroy(dd);
        }
#if MBEVOCODER_CHIP_MEASURED
        // An AMBE+2 silence frame (DTX) is rendered from the last voice model and is not the
        // source decoder's prediction state, as on the AMBE-3000R: the model the transcoder
        // reports after it is still the voice frame's, and the frame transcodes as voice.
        for (mbe_rate to : {MBE_RATE_AMBE3600x2400, MBE_RATE_AMBE3600x2450}) {
            mbe_transcoder* tc = mbe_transcoderCreate(MBE_RATE_AMBE2450, to);
            CHECK(tc != nullptr);
            int bv[9] = {64, 0, 30, 100, 40, 8, 4, 4, 2}, bs[9] = {124, 16, 0, 87, 77, 8, 14, 13, 7};
            char v[49], sil[49];
            CHECK_EQ(mbe_packAmbe2450Fields(bv, v), 0);
            CHECK_EQ(mbe_packAmbe2450Fields(bs, sil), 0);
            for (int i = 0; i < 3; ++i) CHECK_EQ(mbe_transcoderProcess(tc, v, o), MBE_TRANSCODE_VOICE);
            const float w0V = mbe_transcoderModel(tc)->w0, gV = mbe_transcoderModel(tc)->gamma;
            for (int i = 0; i < 3; ++i) CHECK_EQ(mbe_transcoderProcess(tc, sil, o), MBE_TRANSCODE_VOICE);
            CHECK(std::fabs(mbe_transcoderModel(tc)->w0 - w0V) < 1e-6f);
            CHECK(std::fabs(mbe_transcoderModel(tc)->gamma - gV) < 1e-6f);
            CHECK_EQ(mbe_transcoderProcess(tc, v, o), MBE_TRANSCODE_VOICE);
            CHECK(std::fabs(mbe_transcoderModel(tc)->w0 - w0V) < 1e-6f);
            mbe_transcoderDestroy(tc);
        }
#endif
        if (!knox.empty()) {
            mbe_transcoder* raw = mbe_transcoderCreate(MBE_RATE_AMBE3600x2450, MBE_RATE_AMBE2450);
            CHECK_EQ(mbe_transcoderProcess(raw, knox[30].data(), o), MBE_TRANSCODE_TONE);
            mbe_transcoderDestroy(raw);
        }
    }
}

} // namespace

// AMBE+2 to IMBE: every source fundamental within IMBE's range comes out on the nearest IMBE
// index, so the decoded period is within half a step (a quarter sample) of the source's. The
// transcoder used to round the period to half samples, which coded round(2p) - 39 and decoded
// a quarter sample long on average, up to half a sample.
void transcoderImbePitch()
{
    mbe_transcoder* t = mbe_transcoderCreate(MBE_RATE_AMBE2450, MBE_RATE_IMBE4400);
    CHECK(t != nullptr);
    if (!t) return;
    int checked = 0, onGrid = 0;
    double worst = 0.0;
    for (int b0 = 0; b0 < 120; ++b0) {
        int b[9] = {b0, 2, 20, 100, 50, 10, 9, 8, 4};
        char din[49], dout[88];
        CHECK_EQ(mbe_packAmbe2450Fields(b, din), 0);
        if (mbe_transcoderProcess(t, din, dout) != MBE_TRANSCODE_VOICE) continue;
        const double ps = 2.0 * 3.14159265358979 / mbe_transcoderModel(t)->w0;
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        CHECK(mbe_decodeImbe4400Parms(dout, &cur, &prev) >= 0);
        const double pd = 2.0 * 3.14159265358979 / cur.w0;
        if (ps < 19.75 + 0.25 || ps > 123.25 - 0.25) continue;   // outside IMBE's range: clamped
        ++checked;
        const double err = std::fabs(pd - ps);
        worst = std::max(worst, err);
        onGrid += err <= 0.25 + 1e-3;
    }
    std::printf("transcoder AMBE+2 -> IMBE pitch: %d fundamentals, worst period error %.3f samples\n", checked, worst);
    CHECK(checked > 100);
    CHECK_EQ(onGrid, checked);
    mbe_transcoderDestroy(t);
}

int main(int argc, char** argv)
{
    g_dataDir = argc > 1 ? argv[1] : "tests/data/tones";
    transcoderImbePitch();
    if (argc > 2) transcoder(); else { toneFrames(); transcoder(); }
    return test::finish("test_tones_transcode");
}
