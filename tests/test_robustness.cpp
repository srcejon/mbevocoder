// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder robustness over long streams and odd frame sequences:
// state that used to grow without bound or go non-finite.
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

#if MBEVOCODER_CHIP_MEASURED
bool finiteModel(const mbe_parms& p)
{
    if (!std::isfinite(p.w0) || !std::isfinite(p.gamma)) return false;
    for (int l = 0; l <= 56; ++l)
        if (!std::isfinite(p.Ml[l]) || !std::isfinite(p.log2Ml[l])) return false;
    return true;
}
#endif

bool finite(const float* x)
{
    for (int i = 0; i < 160; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

void packVoice(char d[49])
{
    const int b[9] = {40, 2, 20, 100, 50, 10, 9, 8, 4};
    CHECK_EQ(mbe_packAmbe2450Fields(b, d), 0);
}

// the adaptive amplitude threshold accumulates on every errored frame. Started just below
// INT_MAX (where hours of marginal reception took it), one more errored frame overflowed int;
// now it saturates and the frame is still rendered.
void adaptiveThreshold()
{
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    char d[49];
    packVoice(d);
    float out[160];
    for (int i = 0; i < 4; ++i) CHECK(mbe_processAmbe2450Dataf(out, nullptr, d, &cur, &prev, &enh) >= 0);
    // the synthesis reads the previous frame's threshold from the enhanced state
    cur.amplitudeThreshold = prev.amplitudeThreshold = enh.amplitudeThreshold = INT_MAX - 100;
    mbe_process_result r;
    mbe_initProcessResult(&r);
    r.protected_errors = 3;   // a few errors: not repeated, but enough to drive the smoothing
    r.total_errors = 3;
    cur.errorRate = prev.errorRate = enh.errorRate = 0.05f;
    CHECK(mbe_processAmbe2450Dataf(out, &r, d, &cur, &prev, &enh) >= 0);
    CHECK(finite(out));
    CHECK_EQ(enh.amplitudeThreshold, INT_MAX);   // saturated, where int overflowed
}

// an AMBE+2 erasure frame followed by repeats (C0 errors, or lost frames) repeats a model
// with w0 = 0, whose spectral enhancement was 0/0. Output and model stay finite and the stream
// recovers on the next good frame.
void repeatedErasure()
{
    for (int lost = 0; lost < 2; ++lost) {
        for (int atStart = 0; atStart < 2; ++atStart) {
            mbe_parms cur, prev, enh;
            mbe_initMbeParms(&cur, &prev, &enh);
            char d[49], fr[4][24], dd[49];
            float out[160];
            auto frame = [&](const int* b, int flips) {
                CHECK_EQ(mbe_packAmbe2450Fields(b, d), 0);
                mbe_encodeAmbe3600x2450Frame(d, fr);
                for (int k = 0; k < flips; ++k) fr[0][2 + 5 * k] ^= 1;
                mbe_process_result r;
                mbe_initProcessResult(&r);
                CHECK(mbe_processAmbe3600x2450Framef(out, &r, fr, dd, &cur, &prev, &enh) >= 0);
                CHECK(finite(out));
            };
            const int e[9] = {120, 0, 0, 0, 0, 0, 0, 0, 0};
            const int v[9] = {40, 2, 20, 100, 50, 10, 9, 8, 4};
            if (!atStart) frame(v, 0);
            frame(e, 0);
            for (int k = 0; k < 2; ++k) {
                if (lost) {
                    mbe_process_result r;
                    CHECK(mbe_processLostFramef(out, &r, &cur, &prev, &enh) >= 0);
                    CHECK(finite(out));
                } else {
                    frame(v, 4);   // C0 uncorrectable: repeated
                }
#if MBEVOCODER_CHIP_MEASURED
                CHECK(finiteModel(cur) && finiteModel(prev) && finiteModel(enh));
#endif
            }
            double energy = 0.0;
            for (int k = 0; k < 4; ++k) {
                frame(v, 0);
                for (int i = 0; i < 160; ++i) energy += static_cast<double>(out[i]) * out[i];
            }
            CHECK(energy > 0.0);   // recovered
        }
    }
}

// a long run of lost frames holds the mute without the repeat count growing.
void lostFrameCount()
{
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    char d[49];
    packVoice(d);
    float out[160];
    for (int i = 0; i < 3; ++i) CHECK(mbe_processAmbe2450Dataf(out, nullptr, d, &cur, &prev, &enh) >= 0);
    mbe_process_result r;
    for (int i = 0; i < 50; ++i) CHECK(mbe_processLostFramef(out, &r, &cur, &prev, &enh) >= 0);
    CHECK(r.flags & MBE_PROCESS_FLAG_MUTE);
    CHECK(cur.repeatCount <= MBE_MAX_FRAME_REPEATS + 1);
#if MBEVOCODER_CHIP_MEASURED
    for (int i = 0; i < 160; ++i) CHECK_EQ(out[i], 0.0f);   // silence (mbelib-neo: comfort noise)
#endif
}

#if MBEVOCODER_CHIP_MEASURED
double energy(const float* x)
{
    double e = 0.0;
    for (int i = 0; i < 160; ++i) e += static_cast<double>(x[i]) * x[i];
    return e;
}

// soft-decision C0 (AMBE+2). Four errors in C0 make the hard word a detected-uncorrectable
// pattern; on weak bits the soft search recovers the word and the frame decodes, on strong bits
// it stays uncorrectable (c0_errors 4: repeated).
void softC0()
{
    char d[49], fr[4][24], dd[49], dclean[49];
    packVoice(d);
    CHECK_EQ(mbe_encodeAmbe3600x2450Frame(d, fr), 0);
    mbe_process_result r;
    CHECK(mbe_decodeAmbe3600x2450Frame(fr, dclean, &r) >= 0);
    for (int strong = 0; strong < 2; ++strong) {
        mbe_soft_bit sfr[4][24];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 24; ++j) sfr[i][j] = mbe_softBitFromHard(fr[i][j], 200);
        for (int k = 0; k < 4; ++k) {   // four flipped C0 bits
            mbe_soft_bit& b = sfr[0][22 - 3 * k];
            b.bit ^= 1u;
            b.reliability = strong ? 200 : 20;
        }
        mbe_initProcessResult(&r);
        CHECK(mbe_decodeAmbe3600x2450SoftFrame(sfr, dd, &r) >= 0);
        if (strong) {
            CHECK_EQ(r.c0_errors, 4);
        } else {
            CHECK(r.c0_errors < 4);
            CHECK(std::memcmp(dd, dclean, 49) == 0);
        }
    }
}

// IMBE 7200 soft decisions: a Golay word the soft search decodes past three bit flips counts as
// a hard decoder's worst correctable word when the flips it overrules are weak, and in full when
// they are not (imbe_frame.cpp, softWordErrors).
void softImbeCount()
{
    char d[88], fr[8][23], dclean[88], dd[88];
    for (int i = 0; i < 88; ++i) d[i] = static_cast<char>((i * 7 + 3) % 5 < 2);
    CHECK_EQ(mbe_encodeImbe7200x4400Frame(d, fr), 0);
    mbe_process_result r;
    CHECK(mbe_decodeImbe7200x4400Frame(fr, dclean, &r) >= 0);
    CHECK_EQ(r.total_errors, 0);
    for (int weakRel : {20, 70}) {
        mbe_soft_bit sfr[8][23];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 23; ++j) sfr[i][j] = mbe_softBitFromHard(fr[i][j], 200);
        for (int k = 0; k < 5; ++k) {   // five flips in C1, beyond a hard decoder's three
            mbe_soft_bit& b = sfr[1][2 + 4 * k];
            b.bit ^= 1u;
            b.reliability = static_cast<uint8_t>(weakRel);
        }
        mbe_initProcessResult(&r);
        CHECK(mbe_decodeImbe7200x4400SoftFrame(sfr, dd, &r) >= 0);
        CHECK(std::memcmp(dd, dclean, 88) == 0);   // the soft search finds the word either way
        // 5 x 20 overrules less than twice the mean reliability (about 178): trusted, counted as 3;
        // 5 x 70 = 350 more than twice the mean (about 171): the full distance
        CHECK_EQ(r.total_errors, weakRel == 20 ? 3 : 5);
    }
}

// D-STAR voice frames for the tone and mute tests below: a steady voice
void packDstarVoice(char d[49])
{
    const int b[9] = {40, 6, 30, 200, 60, 8, 7, 9, 4};
    CHECK_EQ(mbe_packAmbe2400Fields(b, d), 0);
}

void dstarFrame(const char d[49], int c0Flips, char fr[4][24])
{
    CHECK_EQ(mbe_encodeAmbe3600x2400Frame(d, fr), 0);
    for (int k = 0; k < c0Flips; ++k) fr[0][22 - 3 * k] ^= 1;   // C0 data bits
}

// a D-STAR tone frame whose C0 the FEC corrects plays as a tone, whatever the count (the
// AMBE-3000R's rule); one it cannot render (an invalid index) repeats the last frame. Neither
// resets the predictor: the voice frames after come out as they do around a clean tone frame.
void dstarToneFrames()
{
    char voice[49], tone[49], bad[49];
    packDstarVoice(voice);
    CHECK_EQ(mbe_encodeAmbe2400ToneData(50, 100, tone), 0);
    std::memcpy(bad, tone, 49);
    bad[9] = bad[42] = bad[43] = bad[10] = bad[11] = 1;   // index bits 4..0 set: beyond 122 / the dual tones
    bad[6] = 1; bad[7] = 1; bad[8] = 1;                       // def 7: bits 7..5 = 1,0,1 -> index 191
    auto run = [&](const char* special, int c0Flips, unsigned* flagsOut, double* after) {
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        char fr[4][24], dd[49];
        float out[160];
        *after = 0.0;
        for (int i = 0; i < 20; ++i) {
            mbe_process_result r;
            mbe_initProcessResult(&r);
            if (i == 10) dstarFrame(special, c0Flips, fr); else dstarFrame(voice, 0, fr);
            CHECK(mbe_processAmbe3600x2400Framef(out, &r, fr, dd, &cur, &prev, &enh) >= 0);
            if (i == 10) *flagsOut = r.flags;
            if (i >= 12 && i < 16) *after += energy(out);   // past frame 11, where a tone's tail fades out
        }
    };
    unsigned flags = 0, flagsClean = 0;
    double afterClean = 0.0, after = 0.0;
    run(tone, 0, &flagsClean, &afterClean);
    CHECK(flagsClean & MBE_PROCESS_FLAG_TONE);
    run(tone, 2, &flags, &after);   // C0 corrected with 2 errors: JMBE's rule rejected it
    CHECK(flags & MBE_PROCESS_FLAG_TONE);
    CHECK(after > 0.5 * afterClean && after < 2.0 * afterClean);
    run(bad, 0, &flags, &after);
    CHECK(flags & MBE_PROCESS_FLAG_REPEAT);
    CHECK(after > 0.5 * afterClean && after < 2.0 * afterClean);
}

// the first frame decoded after a lost-frame mute comes out near the level of the same frame
// in an unbroken stream (it used to carry the repeated model's tails at full level, 3-8 dB high).
void afterMute()
{
    char d[49];
    packVoice(d);
    auto run = [&](bool gap) {
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        float out[160];
        double e = 0.0;
        for (int i = 0; i < 20; ++i) {
            mbe_process_result r;
            if (gap && i >= 6 && i < 12) CHECK(mbe_processLostFramef(out, &r, &cur, &prev, &enh) >= 0);
            else CHECK(mbe_processAmbe2450Dataf(out, nullptr, d, &cur, &prev, &enh) >= 0);
            if (i == 12) e = energy(out);
        }
        return e;
    };
    const double steady = run(false), resumed = run(true);
    const double db = 10.0 * std::log10(resumed / steady);
    std::printf("first frame after a mute: %+.1f dB against the unbroken stream\n", db);
    CHECK(std::fabs(db) < 2.0);
}
#endif

} // namespace

int main()
{
#if MBEVOCODER_CHIP_MEASURED
    softC0();
    softImbeCount();
    dstarToneFrames();
    afterMute();
#endif
    adaptiveThreshold();
    repeatedErasure();
    lostFrameCount();
    return test::finish("test_robustness");
}
