// SPDX-License-Identifier: GPL-3.0-or-later
// D-STAR floating-point analysis (src/encoder/dstar_analysis.cpp): the stage on its own and
// through the encoder. The byte-level equivalence with the offline reference is checked by
// tools/quality/dstar_ladder.py on the corpus; this covers behaviour.
#include "encoder/ambe_quant.hpp"
#include "encoder/dstar_analysis.hpp"
#include "analysis/imbe_vocoder/imbe_vocoder.h"
#include "test_util.hpp"

#include <mbevocoder/mbevocoder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979;

// A steady voice: harmonics of f0 with a falling spectrum.
std::vector<short> voice(double f0, int frames, double level = 3000.0)
{
    std::vector<short> x(static_cast<size_t>(frames) * 160);
    for (size_t n = 0; n < x.size(); ++n) {
        double v = 0.0;
        for (int h = 1; h * f0 < 3600.0; ++h) v += std::sin(2.0 * kPi * f0 * h * static_cast<double>(n) / 8000.0 + 0.3 * h) / h;
        x[n] = static_cast<short>(level * v);
    }
    return x;
}

// Frequency (Hz) of a D-STAR fundamental index.
double hz(int b0) { return static_cast<double>(mbe::ambe2400Tables().f0(b0)) * 8000.0; }

// The stage on its own: silence never moves the pitch; a clear voice pulls a wrong pitch index
// (one not harmonically related to the voice, where the input is not periodic) back to the
// voice's and keeps a correct one; an exact octave below is left alone (the input is as
// periodic at twice the period, so the gate's required gain over the current pitch is not met;
// the reference behaves the same); an unvoiced row never moves.
void stage()
{
    const auto& t = mbe::ambe2400Tables();
    float lsa[56], target[56];
    for (float& v : lsa) v = 3.0f;
    const int voicedRow = 15, unvoicedRow = 0;   // AmbePlusVuv: 15 all voiced, 0 all unvoiced
    {
        mbe::DstarAnalysis a;
        short zeros[160] = {0};
        for (int i = 0; i < 10; ++i) {
            a.push(zeros);
            const int L = static_cast<int>(t.Ltable[60]);
            CHECK_EQ(a.frame(t, 60, voicedRow, L, lsa, target), 60);
            for (int l = 0; l < L; ++l) CHECK(std::isfinite(target[l]) && std::fabs(target[l] - 3.0f) < 1e-4f);   // flat: mean kept
        }
    }
    auto nearestIndex = [](double f) {
        int best = 0;
        for (int i = 1; i < 120; ++i) if (std::fabs(std::log2(hz(i) / f)) < std::fabs(std::log2(hz(best) / f))) best = i;
        return best;
    };
    for (double f0 : {110.0, 150.0, 210.0}) {
        // the index nearest the voice, one a tritone below it, and one an octave below it
        const int right = nearestIndex(f0), wrong = nearestIndex(f0 / std::sqrt(2.0)), octave = nearestIndex(f0 / 2);
        const std::vector<short> x = voice(f0, 14);
        mbe::DstarAnalysis a;
        int moved = -1, kept = -1, unvoicedKept = -1, octaveKept = -1;
        for (int i = 0; i < 14; ++i) {
            a.push(&x[static_cast<size_t>(i) * 160]);
            if (i < 8) continue;   // windows filled
            if (i == 8) moved = a.frame(t, wrong, voicedRow, static_cast<int>(t.Ltable[wrong]), lsa, target);
            if (i == 9) kept = a.frame(t, right, voicedRow, static_cast<int>(t.Ltable[right]), lsa, target);
            if (i == 10) unvoicedKept = a.frame(t, wrong, unvoicedRow, static_cast<int>(t.Ltable[wrong]), lsa, target);
            if (i == 12) octaveKept = a.frame(t, octave, voicedRow, static_cast<int>(t.Ltable[octave]), lsa, target);
        }
        std::printf("dstar analysis: %.0f Hz voice: index %d (%.1f Hz) -> %d (%.1f Hz); correct %d kept as %d; unvoiced row keeps %d; octave %d (%.1f Hz) -> %d\n",
                    f0, wrong, hz(wrong), moved, hz(moved), right, kept, unvoicedKept, octave, hz(octave), octaveKept);
        // moved to a period at which the voice is periodic: its own, or (as periodic, and within
        // the penalties' tie when the wrong index sits half-way in octaves) the octave below
        CHECK(std::fabs(std::log2(hz(moved) / f0)) < 0.05 || std::fabs(std::log2(hz(moved) / (f0 / 2))) < 0.05);
        CHECK(std::fabs(std::log2(hz(kept) / f0)) < 0.05);
        CHECK_EQ(unvoicedKept, wrong);                        // an unvoiced frame never moves
        if (std::fabs(std::log2(hz(octave) / (f0 / 2))) < 0.05)   // the table reaches the octave below
            CHECK_EQ(octaveKept, octave);                     // documented: exact octave errors stay
    }
}

// Any candidate grid (select): IMBE's 208 transmitted periods (b0 + 39.5) / 2 on the AMBE+2 /
// IMBE framing. A wrong period moves to the voice's (or the octave below, as in stage()); a
// correct one stays; a frame the caller calls unvoiced never moves.
void grid()
{
    constexpr int kCount = 208;
    double w0s[kCount];
    int Ls[kCount];
    for (int i = 0; i < kCount; ++i) {
        w0s[i] = 4.0 * kPi / (i + 39.5);
        Ls[i] = static_cast<int>(0.9254 * std::floor((i + 39.5) / 2.0 + 0.5));
        if (Ls[i] > 56) Ls[i] = 56;
    }
    auto hzOf = [&](int i) { return w0s[i] * 8000.0 / (2.0 * kPi); };
    auto nearest = [&](double f) {
        int best = 0;
        for (int i = 1; i < kCount; ++i) if (std::fabs(std::log2(hzOf(i) / f)) < std::fabs(std::log2(hzOf(best) / f))) best = i;
        return best;
    };
    float lsa[56], target[56];
    for (float& v : lsa) v = 5.0f;
    for (double f0 : {120.0, 180.0}) {
        const int right = nearest(f0), wrong = nearest(f0 / std::sqrt(2.0));
        const std::vector<short> x = voice(f0, 14);
        mbe::DstarAnalysis a;
        a.setFrameAlign(48);
        int moved = -1, kept = -1, unvoicedKept = -1;
        for (int i = 0; i < 14; ++i) {
            a.push(&x[static_cast<size_t>(i) * 160]);
            if (i == 8) moved = a.select(w0s, Ls, kCount, wrong, true, Ls[wrong], lsa, target);
            if (i == 9) kept = a.select(w0s, Ls, kCount, right, true, Ls[right], lsa, target);
            if (i == 10) unvoicedKept = a.select(w0s, Ls, kCount, wrong, false, Ls[wrong], lsa, target);
        }
        std::printf("dstar analysis: IMBE grid, %.0f Hz voice: %d (%.1f Hz) -> %d (%.1f Hz); correct %d kept as %d; unvoiced keeps %d\n",
                    f0, wrong, hzOf(wrong), moved, hzOf(moved), right, kept, unvoicedKept);
        CHECK(std::fabs(std::log2(hzOf(moved) / f0)) < 0.05 || std::fabs(std::log2(hzOf(moved) / (f0 / 2))) < 0.05);
        CHECK(std::fabs(std::log2(hzOf(kept) / f0)) < 0.05);
        CHECK_EQ(unvoicedKept, wrong);
        double mean = 0.0;   // the target keeps the caller's mean
        for (int l = 0; l < Ls[wrong]; ++l) mean += target[l];
        CHECK(std::fabs(mean / Ls[wrong] - 5.0) < 1e-3);
    }
}

#if MBEVOCODER_DSTAR_PITCH_ANALYSIS || MBEVOCODER_AMBE2450_PITCH_ANALYSIS || MBEVOCODER_IMBE_PITCH_ANALYSIS
// Through the encoder at one rate: reset reproduces the stream (the analysis history is reset
// too) and the coded pitch follows steady voices. `pitchHz` maps a frame's parameters
// (mbe_encoderEncodeParms' b[]) to the coded fundamental.
template <typename PitchHz>
void steadyVoices(mbe_rate rate, const char* name, PitchHz pitchHz)
{
    mbe_encoder_options o;
    mbe_encoderDefaultOptions(&o);
    for (double f0 : {110.0, 150.0, 210.0, 260.0}) {
        const std::vector<short> x = voice(f0, 60);
        mbe_encoder* e = mbe_encoderCreate(rate, &o);
        std::vector<double> coded;
        std::vector<char> first;
        for (int pass = 0; pass < 2; ++pass) {
            mbe_encoderReset(e);
            std::vector<char> all;
            for (int i = 0; i < 60; ++i) {
                char d[88] = {0};   // AMBE rates fill 49
                int b[9];
                CHECK_EQ(mbe_encoderEncodeParms(e, &x[static_cast<size_t>(i) * 160], d, b), 0);
                all.insert(all.end(), d, d + 88);
                if (pass == 0 && i >= 10) coded.push_back(pitchHz(b));
            }
            if (pass == 0) first = all;
            else CHECK(all == first);
        }
        int near = 0;
        for (double f : coded) near += std::fabs(std::log2(f / f0)) < 0.05;
        std::printf("dstar analysis: %s encoder on a %.0f Hz voice: %d of %zu frames within a semitone\n", name, f0, near, coded.size());
        CHECK(near >= static_cast<int>(coded.size()) * 9 / 10);
        mbe_encoderDestroy(e);
    }
}
#endif

#if MBEVOCODER_DSTAR_PITCH_ANALYSIS
// D-STAR: steady voices, and tone frames still pass.
void encoder()
{
    steadyVoices(MBE_RATE_AMBE3600x2400, "D-STAR", [](const int* b) { return hz(b[0]); });
    mbe_encoder_options o;
    mbe_encoderDefaultOptions(&o);
    // a DTMF digit is still a tone frame
    {
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE3600x2400, &o);
        int tones = 0;
        for (int i = 0; i < 30; ++i) {
            short pcm[160];
            for (int n = 0; n < 160; ++n) {
                const double tt = (i * 160 + n) / 8000.0;
                pcm[n] = static_cast<short>(3000.0 * (std::sin(2 * kPi * 770 * tt) + std::sin(2 * kPi * 1336 * tt)));
            }
            char d[49];
            CHECK(mbe_encoderEncodeParms(e, pcm, d, nullptr) >= 0);
            tones += (mbe_encoderFrameFlags(e) & MBE_ENCODER_FLAG_TONE) != 0;
        }
        CHECK(tones >= 20);
        mbe_encoderDestroy(e);
    }
}
#endif

} // namespace

// The IMBE analysis' voicing scale (imbe_vocoder::set_voicing_scale, MBEVOCODER_IMBE_VOICING_THRESHOLD):
// on a voice half buried in noise, a higher threshold voices more harmonics and never fewer,
// and 100 is the standard's analysis unchanged.
void imbeVoicingScale()
{
    std::vector<short> x = voice(140.0, 50, 2000.0);
    unsigned seed = 12345u;
    for (short& s : x) {
        seed = seed * 1103515245u + 12345u;
        const int noise = static_cast<int>((seed >> 16) & 0x7FFF) - 16384;
        s = static_cast<short>(std::max(-32768, std::min(32767, s + noise / 5)));
    }
    auto voiced = [&](int pct, std::vector<int>& perFrame) {
        imbe_vocoder v;
        if (pct) v.set_voicing_scale(static_cast<int16_t>(pct));
        int16_t fv[8];
        int total = 0;
        for (size_t f = 0; f + 160 <= x.size(); f += 160) {
            v.imbe_encode(fv, x.data() + f);
            const IMBE_PARAM* p = v.param();
            int n = 0;
            for (int l = 0; l < p->num_harms; ++l) n += p->v_uv_dsn[l] != 0;
            perFrame.push_back(n);
            total += n;
        }
        return total;
    };
    std::vector<int> byDefault, at100, at175;
    const int d = voiced(0, byDefault), n100 = voiced(100, at100), n175 = voiced(175, at175);
    CHECK(byDefault == at100);
    CHECK(d == n100);
    CHECK(n175 > n100);
    CHECK(n100 > 0);   // the voice is detectable at all
}

// A stream past 2^31 samples (74.6 hours) analyses exactly as a fresh one: the sample counters
// are 64-bit (a 32-bit `long` wrapped negative and indexed before the
// history buffers). The second analysis starts 2048 samples short of 2^31 and runs across it.
void counterWrap()
{
    const auto& t = mbe::ambe2400Tables();
    float lsa[56], ta[56], tb[56];
    for (float& v : lsa) v = 3.0f;
    const std::vector<short> x = voice(150.0, 40);
    mbe::DstarAnalysis a, b;
    b.advanceSampleCountForTest((int64_t{1} << 31) - 2 * mbe::DstarAnalysis::kHistory);
    int same = 0, frames = 0;
    for (int i = 0; i < 40; ++i) {
        a.push(&x[static_cast<size_t>(i) * 160]);
        b.push(&x[static_cast<size_t>(i) * 160]);
        const int b0 = 40 + i % 30, L = static_cast<int>(t.Ltable[b0]);
        const int ra = a.frame(t, b0, 15, L, lsa, ta), rb = b.frame(t, b0, 15, L, lsa, tb);
        const int Ln = static_cast<int>(t.Ltable[ra]);
        ++frames;
        same += ra == rb && std::memcmp(ta, tb, sizeof(float) * static_cast<size_t>(Ln)) == 0;
    }
    CHECK_EQ(same, frames);
}

int main()
{
    stage();
    counterWrap();
    imbeVoicingScale();
    grid();
#if MBEVOCODER_DSTAR_PITCH_ANALYSIS
    encoder();   // the encoder uses the stage only when the library is built with it
#endif
#if MBEVOCODER_AMBE2450_PITCH_ANALYSIS
    steadyVoices(MBE_RATE_AMBE3600x2450, "AMBE+2",
                 [](const int* b) { return static_cast<double>(mbe::ambe2450Tables().f0(b[0])) * 8000.0; });
#endif
#if MBEVOCODER_IMBE_PITCH_ANALYSIS
    // IMBE: b[] holds the codewords u0..u7; b0 is u0's top six bits and u7's bits 2..1, and
    // the period is (b0 + 39.5) / 2 samples
    steadyVoices(MBE_RATE_IMBE7200x4400, "IMBE", [](const int* b) {
        const int b0 = ((b[0] >> 4) & 0xFC) | ((b[7] >> 1) & 0x03);
        return 16000.0 / (b0 + 39.5);
    });
#endif
    return test::finish("test_dstar_analysis");
}
