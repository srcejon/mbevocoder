// SPDX-License-Identifier: GPL-3.0-or-later
// Functional encoder tests: API contract, determinism, pitch tracking and level through an
// encode/decode round trip, silence handling, layouts and the IMBE path.
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

static const float kPi = 3.14159265358979f;

// A vowel-like signal: harmonics of f0 with a slow amplitude modulation, peak ~ 8000.
static std::vector<short> synthVoice(float f0, float seconds, float amp = 8000.f)
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

static double rms(const float* x, int n) { double e = 0; for (int i = 0; i < n; ++i) e += x[i] * x[i]; return std::sqrt(e / n); }

static void apiContract()
{
    CHECK(mbe_encoderCreate(MBE_RATE_COUNT, nullptr) == nullptr);
    mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, nullptr);
    CHECK(e != nullptr);
    CHECK_EQ(mbe_encoderRate(e), MBE_RATE_AMBE3600x2450);
    CHECK_EQ(mbe_encoderDelaySamples(e), 189);
    {
        mbe_encoder* ds = mbe_encoderCreate(MBE_RATE_AMBE3600x2400, nullptr);
        CHECK_EQ(mbe_encoderDelaySamples(ds), 239);   // the AMBE-3000R's D-STAR framing is 50 samples later
        mbe_encoderDestroy(ds);
    }
    CHECK_EQ(mbe_encoderParmBits(MBE_RATE_AMBE2450), 49);
    CHECK_EQ(mbe_encoderParmBits(MBE_RATE_IMBE4400), 88);
    CHECK_EQ(mbe_encoderParmBits(MBE_RATE_COUNT), 0);
    int bad = -1;
    CHECK_EQ(mbe_encoderParmBits(static_cast<mbe_rate>(bad)), 0);
    short pcm[160] = {0};
    char d[49];
    CHECK_EQ(mbe_encoderEncodeParms(nullptr, pcm, d, nullptr), MBE_STATUS_INVALID_ARGUMENT);
    CHECK_EQ(mbe_encoderEncodeParms(e, nullptr, d, nullptr), MBE_STATUS_INVALID_ARGUMENT);
    char fr8[8][23];
    CHECK_EQ(mbe_encoderEncodeImbeFrame(e, pcm, fr8), MBE_STATUS_INVALID_ARGUMENT);   // wrong rate
    mbe_encoderDestroy(e);
    mbe_encoderDestroy(nullptr);
}

static void comfortNoiseArguments()
{
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    const mbe_parms savedCur = cur, savedPrev = prev, savedEnh = enh;
    for (int missing = 0; missing < 3; ++missing) {
        short pcm[160];
        for (short& sample : pcm) sample = 12345;
        mbe_synthesizeComfortNoiseFrame(pcm, missing == 0 ? nullptr : &cur,
                                       missing == 1 ? nullptr : &prev,
                                       missing == 2 ? nullptr : &enh);
        for (short sample : pcm) CHECK_EQ(sample, 12345);
        CHECK_EQ(std::memcmp(&cur, &savedCur, sizeof cur), 0);
        CHECK_EQ(std::memcmp(&prev, &savedPrev, sizeof prev), 0);
        CHECK_EQ(std::memcmp(&enh, &savedEnh, sizeof enh), 0);
    }
    mbe_synthesizeComfortNoiseFrame(nullptr, &cur, &prev, &enh);
    short pcm[160];
    mbe_synthesizeComfortNoiseFrame(pcm, &cur, &prev, &enh);
}

static void limiter()
{
    // Float input at twice full scale: without the limiter the conversion to 16 bits clips
    // (flat tops, distorted spectrum); with it the frame is scaled to -1 dBFS peak and the
    // analysis amplitudes come out lower.
    auto base = synthVoice(140.f, 0.5f, 8000.f);
    std::vector<float> sig(base.size());
    for (size_t i = 0; i < base.size(); ++i) sig[i] = base[i] * 12.0f;   // peaks near 60000
    mbe_encoder_options opts;
    mbe_encoderDefaultOptions(&opts);
    CHECK_EQ(opts.limiter, 0);
    double gainOff = 0, gainOn = 0;
    for (int pass = 0; pass < 2; ++pass) {
        opts.limiter = pass;
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE2450, &opts);
        int frames = 0;
        for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++frames) {
            char d[49];
            CHECK_EQ(mbe_encoderEncodeParmsf(e, &sig[i], d, nullptr), 0);
            if (frames < 5) continue;
            mbe_parms m;
            mbe_encoderAnalysis(e, &m);
            double s = 0;
            for (int l = 1; l <= m.L; ++l) s += m.log2Ml[l];
            (pass ? gainOn : gainOff) += s / m.L;
        }
        mbe_encoderDestroy(e);
    }
    std::printf("limiter: mean analysis log2 amplitude %.2f off, %.2f on\n", gainOff / 20, gainOn / 20);
    CHECK(gainOn / 20 < gainOff / 20 - 0.5);   // at least 3 dB lower

    // The float entry limits exactly once: the same signal at 12x and at 120x full scale
    // must not come out quieter (a second pass of the limiter would pull the louder one
    // down again), and at a level that fits 16 bits the float and 16-bit entries agree.
    opts.limiter = 1;
    double gain[4] = {0, 0, 0, 0};   // 12x float, 120x float, 4.5x float, 4.5x 16-bit
    for (int pass = 0; pass < 4; ++pass) {
        const float scale = pass == 0 ? 12.0f : pass == 1 ? 120.0f : 4.5f;   // 4.5x peaks near 32000, above the -1 dBFS ceiling
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE2450, &opts);
        int frames = 0;
        for (size_t i = 0; i + 160 <= base.size(); i += 160, ++frames) {
            char d[49];
            float x[160];
            for (int k = 0; k < 160; ++k) x[k] = base[i + k] * scale;
            if (pass == 3) {
                short s16[160];
                for (int k = 0; k < 160; ++k) s16[k] = static_cast<short>(std::lrint(x[k]));
                CHECK_EQ(mbe_encoderEncodeParms(e, s16, d, nullptr), 0);
            } else {
                CHECK_EQ(mbe_encoderEncodeParmsf(e, x, d, nullptr), 0);
            }
            if (frames < 5) continue;
            mbe_parms m;
            mbe_encoderAnalysis(e, &m);
            double sum = 0;
            for (int l = 1; l <= m.L; ++l) sum += m.log2Ml[l];
            gain[pass] += sum / m.L;
        }
        mbe_encoderDestroy(e);
    }
    std::printf("limiter: 12x %.2f, 120x %.2f, 4.5x float %.2f, 4.5x 16-bit %.2f\n", gain[0] / 20, gain[1] / 20, gain[2] / 20, gain[3] / 20);
    CHECK(gain[1] >= gain[0] - 0.05 * 20);   // louder input never comes out quieter
    CHECK(std::fabs(gain[3] - gain[2]) < 0.05 * 20);   // both entries limit the same way, once
}

static void determinismAndSilence()
{
    auto sig = synthVoice(150.f, 1.0f);
    std::vector<char> a, b;
    for (int pass = 0; pass < 2; ++pass) {
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE2450, nullptr);
        std::vector<char>& out = pass ? b : a;
        for (size_t i = 0; i + 160 <= sig.size(); i += 160) {
            char d[49];
            CHECK_EQ(mbe_encoderEncodeParms(e, &sig[i], d, nullptr), 0);
            out.insert(out.end(), d, d + 49);
        }
        mbe_encoderDestroy(e);
    }
    CHECK(a == b);
    // reset restores the initial state
    mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE2450, nullptr);
    std::vector<char> c;
    for (size_t i = 0; i + 160 <= sig.size(); i += 160) { char d[49]; mbe_encoderEncodeParms(e, &sig[i], d, nullptr); }
    mbe_encoderReset(e);
    for (size_t i = 0; i + 160 <= sig.size(); i += 160) { char d[49]; mbe_encoderEncodeParms(e, &sig[i], d, nullptr); c.insert(c.end(), d, d + 49); }
    CHECK(a == c);
    // digital silence yields the canonical silence frame
    short zero[160] = {0};
    char d[49];
    int bb[9];
    for (int i = 0; i < 5; ++i) CHECK_EQ(mbe_encoderEncodeParms(e, zero, d, bb), 0);
    CHECK_EQ(bb[0], 119);
    CHECK_EQ(bb[1], 16);
    CHECK_EQ(bb[2], 0);
    mbe_encoderDestroy(e);
}

static void roundTrip(float f0, bool dstar = false)
{
    auto sig = synthVoice(f0, 2.0f);
    mbe_encoder* e = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, nullptr);
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    int frames = 0, pitchOk = 0, voiced = 0;
    double outE = 0;
    for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++frames) {
        char fr[4][24], d[49];
        CHECK_EQ(mbe_encoderEncodeAmbeFrame(e, &sig[i], fr), 0);
        float pcm[160];
        mbe_process_result r;
        CHECK_EQ((dstar ? mbe_processAmbe3600x2400Framef : mbe_processAmbe3600x2450Framef)(pcm, &r, fr, d, &cur, &prev, &enh), 0);
        CHECK_EQ(r.total_errors, 0);
        if (frames < 10) continue;   // let the analysis window and prediction settle
        outE += rms(pcm, 160);
        bool anyVoiced = false;
        for (int l = 1; l <= cur.L; ++l) if (cur.Vl[l]) anyVoiced = true;
        if (anyVoiced) {   // some band voiced
            ++voiced;
            float f0Dec = cur.w0 * 8000.f / (2 * kPi);
            if (std::fabs(f0Dec - f0) < 0.06f * f0) ++pitchOk;
        }
    }
    mbe_encoderDestroy(e);
    double frac = voiced ? static_cast<double>(pitchOk) / voiced : 0;
    std::printf("round trip %s f0=%.0f Hz: %d frames, %d voiced, pitch within 6%%: %.0f%%, mean out rms %.0f\n",
                dstar ? "D-STAR" : "AMBE+2", f0, frames, voiced, frac * 100, outE / (frames - 10));
    CHECK(voiced > (frames - 10) / 2);
    CHECK(frac > 0.8);
    CHECK(outE / (frames - 10) > (dstar ? 15 : 75));   // decoder output on the float scale (x7 = 16-bit at the AMBE-3000R's level), not silence (D-STAR is level-calibrated 12.8 dB lower)
}

// Quiet input (docs/encoder-quality.md "Quiet input"): the input gain in front of the analysis
// keeps pitch and voicing at -40 dB and takes the gain off again, so the decoded level follows
// the input's. Before it, the analysis lost most voiced frames there.
static void quietInput()
{
    const mbe_rate rates[] = {MBE_RATE_AMBE3600x2450, MBE_RATE_AMBE3600x2400, MBE_RATE_IMBE7200x4400};
    for (mbe_rate rate : rates) {
        double level[2] = {0, 0};
        int voicedFrames[2] = {0, 0}, pitchOk[2] = {0, 0}, frames = 0;
        for (int quiet = 0; quiet < 2; ++quiet) {
            auto sig = synthVoice(150.f, 2.0f, quiet ? 80.f : 8000.f);   // -40 dB
            mbe_encoder* e = mbe_encoderCreate(rate, nullptr);
            mbe_parms cur, prev, enh;
            mbe_initMbeParms(&cur, &prev, &enh);
            frames = 0;
            for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++frames) {
                float pcm[160];
                mbe_process_result r;
                if (rate == MBE_RATE_IMBE7200x4400) {
                    char fr[8][23], d[88];
                    CHECK_EQ(mbe_encoderEncodeImbeFrame(e, &sig[i], fr), 0);
                    CHECK(mbe_processImbe7200x4400Framef(pcm, &r, fr, d, &cur, &prev, &enh) >= 0);
                } else {
                    char fr[4][24], d[49];
                    CHECK_EQ(mbe_encoderEncodeAmbeFrame(e, &sig[i], fr), 0);
                    CHECK(((rate == MBE_RATE_AMBE3600x2400) ? mbe_processAmbe3600x2400Framef : mbe_processAmbe3600x2450Framef)(pcm, &r, fr, d, &cur, &prev, &enh) >= 0);
                }
                if (frames < 10) continue;
                level[quiet] += rms(pcm, 160);
                bool anyVoiced = false;
                for (int l = 1; l <= cur.L; ++l) anyVoiced = anyVoiced || cur.Vl[l];
                if (anyVoiced) {
                    ++voicedFrames[quiet];
                    if (std::fabs(cur.w0 * 8000.f / (2 * kPi) - 150.f) < 9.f) ++pitchOk[quiet];
                }
            }
            mbe_encoderDestroy(e);
        }
        const double ratioDb = 20.0 * std::log10(level[1] / level[0]);
        std::printf("quiet input, rate %d: voiced %d / %d frames, pitch right %d / %d, level %.1f dB\n", static_cast<int>(rate),
                    voicedFrames[1], voicedFrames[0], pitchOk[1], pitchOk[0], ratioDb);
        CHECK(voicedFrames[1] >= voicedFrames[0] * 9 / 10);
        CHECK(pitchOk[1] >= pitchOk[0] * 9 / 10);
        CHECK(std::fabs(ratioDb + 40.0) < 3.0);
    }
}

static void layoutsAndImbe()
{
    auto sig = synthVoice(200.f, 1.0f);
    mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, nullptr);
    for (size_t i = 0; i + 160 <= sig.size(); i += 160) {
        char bits[72], fr[4][24], d[49];
        CHECK_EQ(mbe_encoderEncode(e, &sig[i], MBE_LAYOUT_P25P2, bits), 0);
        CHECK_EQ(mbe_ambeFrameFromBits(MBE_LAYOUT_P25P2, bits, fr), 0);
        mbe_process_result r;
        CHECK_EQ(mbe_decodeAmbe3600x2450Frame(fr, d, &r), 0);
        uint8_t bytes[9];
        CHECK_EQ(mbe_encoderEncodeDvsi(e, &sig[i], bytes), 0);
    }
    mbe_encoderDestroy(e);

    // IMBE path: encode -> 7200x4400 frame -> decode, energy and pitch sanity
    e = mbe_encoderCreate(MBE_RATE_IMBE7200x4400, nullptr);
    CHECK(e != nullptr);
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    int frames = 0, pitchOk = 0;
    double outE = 0;
    for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++frames) {
        char fr[8][23], d[88];
        CHECK_EQ(mbe_encoderEncodeImbeFrame(e, &sig[i], fr), 0);
        float pcm[160];
        mbe_process_result r;
        CHECK_EQ(mbe_processImbe7200x4400Framef(pcm, &r, fr, d, &cur, &prev, &enh), 0);
        CHECK_EQ(r.total_errors, 0);
        if (frames < 10) continue;
        outE += rms(pcm, 160);
        float f0Dec = cur.w0 * 8000.f / (2 * kPi);
        if (std::fabs(f0Dec - 200.f) < 12.f) ++pitchOk;
    }
    mbe_encoderDestroy(e);
    std::printf("imbe round trip: %d frames, pitch within 6%%: %d/%d, mean out rms %.0f\n", frames, pitchOk, frames - 10, outE / (frames - 10));
    CHECK(pitchOk > (frames - 10) * 0.8);
    CHECK(outE / (frames - 10) > 75);

    // IMBE layouts: encode through the layout API, reassemble, decode with zero corrections,
    // and check the two layouts carry the same frame (same bits after re-layout).
    e = mbe_encoderCreate(MBE_RATE_IMBE7200x4400, nullptr);
    mbe_encoder* e2 = mbe_encoderCreate(MBE_RATE_IMBE7200x4400, nullptr);
    for (size_t i = 0; i + 160 <= sig.size(); i += 160) {
        char p25[144], ysf[144], fr[8][23], fr2[8][23], d[88];
        CHECK_EQ(mbe_encoderEncode(e, &sig[i], MBE_LAYOUT_P25P1, p25), 0);
        CHECK_EQ(mbe_encoderEncode(e2, &sig[i], MBE_LAYOUT_YSF_VW, ysf), 0);
        CHECK_EQ(mbe_imbeFrameFromBits(MBE_LAYOUT_P25P1, p25, fr), 0);
        CHECK_EQ(mbe_imbeFrameFromBits(MBE_LAYOUT_YSF_VW, ysf, fr2), 0);
        CHECK_EQ(std::memcmp(fr, fr2, sizeof fr), 0);
        mbe_process_result r;
        CHECK_EQ(mbe_decodeImbe7200x4400Frame(fr, d, &r), 0);
        CHECK_EQ(r.total_errors, 0);
        char back[144];
        CHECK_EQ(mbe_imbeFrameToBits(MBE_LAYOUT_YSF_VW, fr, back), 0);
        CHECK_EQ(std::memcmp(back, ysf, sizeof back), 0);
        char bad[144];   // AMBE layouts are not valid for an IMBE encoder
        CHECK_EQ(mbe_encoderEncode(e, &sig[i], MBE_LAYOUT_DMR, bad), MBE_STATUS_INVALID_ARGUMENT);
    }
    mbe_encoderDestroy(e);
    mbe_encoderDestroy(e2);
    // and within the AMBE family a layout carries one frame: the D-STAR layout is not a DMR frame
    {
        mbe_encoder* ds = mbe_encoderCreate(MBE_RATE_AMBE3600x2400, nullptr);
        mbe_encoder* dm = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, nullptr);
        char bits[72];
        CHECK_EQ(mbe_encoderEncode(ds, &sig[0], MBE_LAYOUT_DMR, bits), MBE_STATUS_INVALID_ARGUMENT);
        CHECK_EQ(mbe_encoderEncode(ds, &sig[0], MBE_LAYOUT_P25P2, bits), MBE_STATUS_INVALID_ARGUMENT);
        CHECK_EQ(mbe_encoderEncode(dm, &sig[0], MBE_LAYOUT_DSTAR, bits), MBE_STATUS_INVALID_ARGUMENT);
        CHECK_EQ(mbe_encoderEncode(ds, &sig[0], MBE_LAYOUT_DSTAR, bits), 0);
        CHECK_EQ(mbe_encoderEncode(dm, &sig[0], MBE_LAYOUT_DMR, bits), 0);
        mbe_encoderDestroy(ds);
        mbe_encoderDestroy(dm);
    }
}

// Tone detection: a 1000 Hz tone, a DTMF digit and a call-progress pair become tone frames
// with the AMBE-3000R's ids and amplitude ids; voice never does; the option turns it off.
static void toneDetection()
{
    auto sine = [](float f1, float f2, float dbfs, int n) {
        std::vector<short> s(n);
        const float a = std::pow(10.0f, dbfs / 20.0f) * 32767.0f;
        for (int i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / 8000.0f;
            float v = f2 > 0 ? 0.5f * (std::sin(2 * kPi * f1 * t) + std::sin(2 * kPi * f2 * t)) : std::sin(2 * kPi * f1 * t);
            s[i] = static_cast<short>(a * v);
        }
        return s;
    };
    struct Case { float f1, f2, dbfs; int id, amp; } cases[] = {
        {1000.f, 0.f, -20.f, 32, 99},         // id 32 = 1000 Hz; AD = 127 - round((0.2 + 20) / 0.714)
        {770.f, 1336.f, -20.f, mbe_toneIdFromFrequencies(770.f, 1336.f), 90},   // DTMF 5, each component at -26 dBFS
        {440.f, 480.f, -20.f, mbe_toneIdFromFrequencies(440.f, 480.f), 90},     // ringback: 40 Hz apart, resolved by the fit
        {2187.5f, 0.f, -35.f, 70, 78},        // quiet tone (the AMBE-3000R stops between -35 and -40 dBFS): id 70, AD 78
    };
    for (bool dstar : {false, true}) {
        mbe_encoder* e = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, nullptr);
        CHECK(e != nullptr);
        CHECK_EQ(mbe_encoderFrameFlags(e), 0);
        for (const Case& c : cases) {
            mbe_encoderReset(e);
            std::vector<short> sig = sine(c.f1, c.f2, c.dbfs, 8000 / 2);
            int toneFrames = 0, ids = -1, amps = -1;
            for (size_t i = 0; i + 160 <= sig.size(); i += 160) {
                char d[49]; int b[9];
                CHECK_EQ(mbe_encoderEncodeParms(e, &sig[i], d, b), 0);
                const int flags = mbe_encoderFrameFlags(e);
                CHECK(flags & MBE_ENCODER_FLAG_VOICE_ACTIVE);
                if (flags & MBE_ENCODER_FLAG_TONE) {
                    ++toneFrames;
                    if (ids < 0) { ids = b[0]; amps = b[1]; } else { CHECK_EQ(b[0], ids); CHECK_EQ(amps, b[1]); }
                    // the bits are the tone frame the builder makes for that id and amplitude
                    char ref[49];
                    CHECK_EQ(dstar ? mbe_encodeAmbe2400ToneData(b[0], b[1], ref) : mbe_encodeAmbe2450ToneData(b[0], b[1], ref), 0);
                    CHECK_EQ(std::memcmp(d, ref, 49), 0);
                }
            }
            // 25 frames minus the two of encoder delay and one for the analysis window to settle
            CHECK(toneFrames >= 20);
            CHECK_EQ(ids, c.id);
            CHECK_EQ(amps, c.amp);
        }
        // voice with the detector on: no tone frames on a low, pure-ish vowel either
        mbe_encoderReset(e);
        std::vector<short> v = synthVoice(200.f, 1.0f);
        for (size_t i = 0; i + 160 <= v.size(); i += 160) {
            char d[49];
            CHECK_EQ(mbe_encoderEncodeParms(e, &v[i], d, nullptr), 0);
            CHECK_EQ(mbe_encoderFrameFlags(e), MBE_ENCODER_FLAG_VOICE_ACTIVE);
        }
        mbe_encoderDestroy(e);
        // detection off: the same tone is coded as voice
        mbe_encoder_options opts;
        mbe_encoderDefaultOptions(&opts);
        CHECK_EQ(opts.tone_detect, 1);
        opts.tone_detect = 0;
        e = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, &opts);
        std::vector<short> sig = sine(1000.f, 0.f, -20.f, 8000 / 2);
        for (size_t i = 0; i + 160 <= sig.size(); i += 160) {
            char d[49];
            CHECK_EQ(mbe_encoderEncodeParms(e, &sig[i], d, nullptr), 0);
            CHECK_EQ(mbe_encoderFrameFlags(e), MBE_ENCODER_FLAG_VOICE_ACTIVE);
        }
        mbe_encoderDestroy(e);
    }
}

// DTX: a vowel with white noise at -40 dBFS behind it, then 1.2 s of the noise alone. The
// voice frames carry the flag, the pause becomes silence frames (fundamental index 124 on
// AMBE+2, 120 on D-STAR, all bands unvoiced) within a few frames, the decoder renders them
// at the background level, and the comfort-noise call for a DTX gap continues that level.
static void dtx()
{
    for (bool dstar : {false, true}) {
        std::vector<short> sig = synthVoice(140.f, 1.5f);
        sig.resize(sig.size() + 8000 * 6 / 5, 0);
        unsigned seed = 12345;
        for (short& v : sig) {   // uniform noise, rms about -40 dBFS
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<short>(v + (static_cast<int>(seed >> 16) % 1135) - 567);
        }
        // reference: the same signal coded as voice throughout (DTX off)
        double refE = 0; int refN = 0;
        {
            mbe_encoder* e0 = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, nullptr);
            mbe_parms c0, p0, e0h;
            mbe_initMbeParms(&c0, &p0, &e0h);
            int f = 0;
            for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++f) {
                char d[49]; float pcm[160];
                CHECK_EQ(mbe_encoderEncodeParms(e0, &sig[i], d, nullptr), 0);
                CHECK_EQ((dstar ? mbe_processAmbe2400Dataf : mbe_processAmbe2450Dataf)(pcm, nullptr, d, &c0, &p0, &e0h), 0);
                if (f >= 90) { refE += rms(pcm, 160); ++refN; }
            }
            mbe_encoderDestroy(e0);
            refE /= refN;
        }
        mbe_encoder_options opts;
        mbe_encoderDefaultOptions(&opts);
        opts.dtx = 1;
        mbe_encoder* e = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, &opts);
        CHECK(e != nullptr);
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        int frame = 0, firstSilence = -1, silenceFrames = 0, voiceAfter = 0;
        double pauseE = 0; int pauseN = 0;
        for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++frame) {
            char d[49]; int b[9];
            CHECK_EQ(mbe_encoderEncodeParms(e, &sig[i], d, b), 0);
            const bool active = (mbe_encoderFrameFlags(e) & MBE_ENCODER_FLAG_VOICE_ACTIVE) != 0;
            float pcm[160];
            CHECK_EQ((dstar ? mbe_processAmbe2400Dataf : mbe_processAmbe2450Dataf)(pcm, nullptr, d, &cur, &prev, &enh), 0);
            if (frame < 70) { CHECK(active); continue; }   // 1.4 s of vowel: voice throughout
            if (!active) {
                if (firstSilence < 0) firstSilence = frame;
                ++silenceFrames;
                CHECK_EQ(b[0], dstar ? 120 : 124);
                CHECK_EQ(b[1], dstar ? 0 : 16);
                for (int l = 1; l <= cur.L; ++l) CHECK_EQ(cur.Vl[l], 0);
                if (frame >= 90) { pauseE += rms(pcm, 160); ++pauseN; }
            } else if (firstSilence >= 0) {
                ++voiceAfter;
            }
        }
        CHECK(firstSilence >= 75 && firstSilence <= 85);   // the vowel ends at frame 75 (+1 of delay), hangover 3
        CHECK(voiceAfter <= 2);
        CHECK(silenceFrames >= 45);
        // the silence frames render the background at the level voice coding gives it (within 4 dB)
        const double bg = pauseE / pauseN;
        std::printf("dtx %s: first silence frame %d, %d silence frames, pause rms %.1f as silence frames, %.1f as voice\n",
                    dstar ? "D-STAR" : "AMBE+2", firstSilence, silenceFrames, bg, refE);
#if MBEVOCODER_CHIP_MEASURED
        CHECK(bg > refE / 1.6 && bg < refE * 1.6);
#else
        if (dstar) CHECK(bg > refE / 1.6 && bg < refE * 1.6);   // JMBE's AMBE+2 silence model renders them 12 dB high
#endif
        // a DTX gap: comfort noise continues at that level and stays there
        double cnE = 0;
        for (int k = 0; k < 25; ++k) {
            float pcm[160];
            mbe_synthesizeComfortNoiseFramef(pcm, &cur, &prev, &enh);
            if (k >= 5) cnE += rms(pcm, 160);
        }
        cnE /= 20;
        std::printf("dtx %s: comfort noise rms %.1f\n", dstar ? "D-STAR" : "AMBE+2", cnE);
        CHECK(cnE > bg / 2.5 && cnE < bg * 2.5);
        // voice resumes cleanly: a frame of the vowel decodes voiced again
        {
            char d[49];
            for (int k = 0; k < 12; ++k) {
                CHECK_EQ(mbe_encoderEncodeParms(e, &sig[static_cast<size_t>(20 + k) * 160], d, nullptr), 0);
                float pcm[160];
                CHECK_EQ((dstar ? mbe_processAmbe2400Dataf : mbe_processAmbe2450Dataf)(pcm, nullptr, d, &cur, &prev, &enh), 0);
            }
            CHECK(mbe_encoderFrameFlags(e) & MBE_ENCODER_FLAG_VOICE_ACTIVE);
            bool anyVoiced = false;
            for (int l = 1; l <= cur.L; ++l) if (cur.Vl[l]) anyVoiced = true;
            CHECK(anyVoiced);
        }
        mbe_encoderDestroy(e);
    }
}

// Lost frames (docs/decoder-quality.md, "Bit errors"): a vowel stream with a run of frames the
// framing layer reports lost. The first three repeat the model at the same level, the fourth mutes
// (fading within the frame), the rest are silence, and the next decoded frame is at its level.
static void lostFrames()
{
    for (bool dstar : {false, true}) {
        std::vector<short> sig = synthVoice(150.f, 1.5f);
        mbe_encoder* e = mbe_encoderCreate(dstar ? MBE_RATE_AMBE3600x2400 : MBE_RATE_AMBE3600x2450, nullptr);
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        float pcm[160];
        auto decode = [&](const char* d) { return (dstar ? mbe_processAmbe2400Dataf : mbe_processAmbe2450Dataf)(pcm, nullptr, d, &cur, &prev, &enh); };
        std::vector<double> level;
        int frame = 0;
        char d[49];
        for (size_t i = 0; i + 160 <= sig.size(); i += 160, ++frame) {
            CHECK_EQ(mbe_encoderEncodeParms(e, &sig[i], d, nullptr), 0);
            if (frame >= 40 && frame < 48) {   // eight lost frames in the middle of the vowel
                mbe_process_result r;
                CHECK_EQ(mbe_processLostFramef(pcm, &r, &cur, &prev, &enh), 0);
                CHECK(r.flags & MBE_PROCESS_FLAG_REPEAT);
                const int n = frame - 40;   // 0..7
                if (n < 3) CHECK(!(r.flags & MBE_PROCESS_FLAG_MUTE)); else CHECK(r.flags & MBE_PROCESS_FLAG_MUTE);
            } else {
                CHECK_EQ(decode(d), 0);
            }
            level.push_back(20 * std::log10(rms(pcm, 160) + 1e-9));
        }
        mbe_encoderDestroy(e);
        const double before = level[39];
        std::printf("lost frames %s: level before %.1f, repeats %.1f %.1f %.1f %.1f, mute %.1f %.1f %.1f %.1f, after %.1f %.1f %.1f (dB)\n",
                    dstar ? "D-STAR" : "AMBE+2", before, level[40], level[41], level[42], level[43], level[44], level[45], level[46], level[47], level[48], level[50], level[55]);
        for (int n = 40; n < 43; ++n) CHECK(std::fabs(level[n] - before) < 3.0);   // three repeats hold the level
#if MBEVOCODER_CHIP_MEASURED
        CHECK(level[43] < before - 3.0 && level[43] > before - 15.0);                // the fourth fades out
        for (int n = 44; n < 48; ++n) CHECK(level[n] < before - 60.0);              // silence
        // The first decoded frame comes out at its own level, that of the frames after it: neither
        // faded in nor carrying the repeated model's tail (which used to lift it towards the
        // pre-loss level). Those frames sit below the pre-loss level here because
        // the encoder kept predicting from the frames that were lost, as it would on air.
        CHECK(std::fabs(level[48] - level[50]) < 3.0);
        CHECK(level[48] > before - 15.0);
#else
        for (int n = 43; n < 48; ++n) CHECK(level[n] < before - 20.0);              // mbelib's comfort noise
        CHECK(level[55] > before - 6.0);                                            // recovered after the state reset
#endif
        // the 16-bit variant and the argument checks
        short s16[160];
        CHECK_EQ(mbe_processLostFrame(s16, nullptr, &cur, &prev, &enh), 0);
        CHECK_EQ(mbe_processLostFramef(nullptr, nullptr, &cur, &prev, &enh), MBE_STATUS_INVALID_ARGUMENT);
        CHECK_EQ(mbe_processLostFramef(pcm, nullptr, nullptr, &prev, &enh), MBE_STATUS_INVALID_ARGUMENT);
    }
}

// Non-finite float input is silence: a frame with NaN and infinite samples
// codes exactly as the same frame with those samples zero, with and without the limiter (whose
// envelope one infinite sample used to hold at infinity, muting the stream until a reset); and
// mbe_toneIdFromFrequencies() rejects NaN.
static void nonFiniteInput()
{
    auto base = synthVoice(140.f, 0.6f, 8000.f);
    std::vector<float> clean(base.begin(), base.end()), dirty = clean;
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (size_t i = 800; i < 960; i += 7) { dirty[i] = (i % 2) ? nan : ((i % 3) ? inf : -inf); clean[i] = 0.0f; }
    for (int limiterOn = 0; limiterOn < 2; ++limiterOn) {
        mbe_encoder_options opts;
        mbe_encoderDefaultOptions(&opts);
        opts.limiter = limiterOn;
        mbe_encoder* a = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, &opts);
        mbe_encoder* b = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, &opts);
        int same = 0, frames = 0;
        for (size_t i = 0; i + 160 <= clean.size(); i += 160, ++frames) {
            char da[49], db[49];
            CHECK_EQ(mbe_encoderEncodeParmsf(a, &dirty[i], da, nullptr), 0);
            CHECK_EQ(mbe_encoderEncodeParmsf(b, &clean[i], db, nullptr), 0);
            same += std::memcmp(da, db, sizeof da) == 0;
        }
        CHECK_EQ(same, frames);
        mbe_encoderDestroy(a);
        mbe_encoderDestroy(b);
    }
    CHECK_EQ(mbe_toneIdFromFrequencies(std::numeric_limits<float>::quiet_NaN(), 0.0f), -1);
    CHECK_EQ(mbe_toneIdFromFrequencies(1000.0f, std::numeric_limits<float>::quiet_NaN()), -1);
}

int main()
{
    apiContract();
    nonFiniteInput();
    comfortNoiseArguments();
    toneDetection();
    dtx();
    lostFrames();
    determinismAndSilence();
    limiter();
    roundTrip(120.f);
    roundTrip(220.f);
    roundTrip(120.f, true);
    quietInput();
    roundTrip(220.f, true);
    layoutsAndImbe();
    return test::finish("test_encoder");
}
