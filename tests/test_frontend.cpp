// SPDX-License-Identifier: GPL-3.0-or-later
// Encoder front end (docs/frontend.md): noise suppression and echo cancellation, measured on
// the input as the analysis sees it (mbe::encoderFrontEndOutput), plus the delay contract.
#include <mbevocoder/mbevocoder.h>

#include "encoder/encoder_internal.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <vector>

static const float kPi = 3.14159265358979f;

static std::vector<short> vowel(float f0, float seconds, float amp)
{
    int n = static_cast<int>(seconds * 8000);
    std::vector<short> s(n);
    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / 8000.f, v = 0;
        for (int h = 1; h <= 20 && f0 * static_cast<float>(h) < 3800; ++h) v += std::sin(2 * kPi * f0 * static_cast<float>(h) * t) / static_cast<float>(h);
        v *= 0.6f + 0.4f * std::sin(2 * kPi * 3.f * t);
        s[i] = static_cast<short>(amp * v / 2.0f);
    }
    return s;
}

static double rmsDb(const short* x, size_t n)
{
    double e = 0;
    for (size_t i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
    return 10 * std::log10(e / static_cast<double>(n) / (32767.0 * 32767.0) + 1e-20);
}

// Run the front end over `in` (with an optional far-end reference) and return what the
// analysis saw, compensated for the front end's delay.
static std::vector<short> frontEnd(const std::vector<short>& in, const mbe_encoder_options& opts, const std::vector<short>* far)
{
    mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, &opts);
    CHECK(e != nullptr);
    const int delay = mbe_encoderDelaySamples(e) - 189;
    std::vector<short> out;
    for (size_t i = 0; i + 160 <= in.size(); i += 160) {
        char d[49];
        if (far) mbe_encoderSetEchoReference(e, &(*far)[i]);
        CHECK_EQ(mbe_encoderEncodeParms(e, &in[i], d, nullptr), 0);
        short fe[160];
        mbe::encoderFrontEndOutput(e, fe);
        out.insert(out.end(), fe, fe + 160);
    }
    mbe_encoderDestroy(e);
    out.erase(out.begin(), out.begin() + delay);
    return out;
}

static void noiseSuppression()
{
    // 1 s of noise, 2 s of a vowel over it, 1 s of noise: the noise floor drops by 6 dB or
    // more, the vowel by 2 dB at most, and the output is aligned with the input
    std::vector<short> v = vowel(150.f, 2.0f, 8000.f);
    std::vector<short> in(8000 * 4, 0);
    unsigned seed = 99;
    for (short& s : in) { seed = seed * 1664525u + 1013904223u; s = static_cast<short>((static_cast<int>(seed >> 16) % 1800) - 900); }   // about -36 dBFS
    for (size_t i = 0; i < v.size(); ++i) in[8000 + i] = static_cast<short>(in[8000 + i] + v[i]);
    mbe_encoder_options opts;
    mbe_encoderDefaultOptions(&opts);
    opts.noise_suppress = 1;
    {
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, &opts);
        CHECK_EQ(mbe_encoderDelaySamples(e), 189 + 160);
        mbe_encoderDestroy(e);
    }
    std::vector<short> out = frontEnd(in, opts, nullptr);
    const double noiseIn = rmsDb(&in[8000 * 3 + 800], 8000 - 800), noiseOut = rmsDb(&out[8000 * 3 + 800], 8000 - 800 - 160);
    const double vowelIn = rmsDb(&in[8000 + 4000], 8000), vowelOut = rmsDb(&out[8000 + 4000], 8000);
    std::printf("noise suppression: noise %.1f -> %.1f dBFS, vowel %.1f -> %.1f dBFS\n", noiseIn, noiseOut, vowelIn, vowelOut);
    CHECK(noiseOut < noiseIn - 6.0);
    CHECK(vowelOut > vowelIn - 2.0);
    // alignment: the vowel's onset appears at the same sample (within a frame)
    size_t onIn = 0, onOut = 0;
    for (size_t i = 8000; i < in.size(); ++i) if (std::abs(in[i]) > 3000) { onIn = i; break; }
    for (size_t i = 8000; i < out.size(); ++i) if (std::abs(out[i]) > 3000) { onOut = i; break; }
    CHECK(onIn > 0 && onOut > 0 && (onOut > onIn ? onOut - onIn : onIn - onOut) < 160);
    // off: the front end passes the input through unchanged
    opts.noise_suppress = 0;
    std::vector<short> same = frontEnd(in, opts, nullptr);
    CHECK_EQ(std::memcmp(same.data(), in.data(), same.size() * sizeof(short)), 0);
}

static void echoCancellation()
{
    // far end: a vowel for 3 s; microphone: its echo through a 3-tap path (6 ms, 10 ms,
    // 14 ms), plus a different vowel from 1.5 s to 2.5 s (double talk)
    std::vector<short> far = vowel(180.f, 3.0f, 10000.f);
    std::vector<short> near = vowel(120.f, 1.0f, 6000.f);
    std::vector<short> mic(far.size(), 0);
    const int taps[3] = {48, 80, 112};
    const float gains[3] = {0.5f, -0.25f, 0.12f};
    for (size_t i = 0; i < mic.size(); ++i) {
        float v = 0;
        for (int k = 0; k < 3; ++k) if (i >= static_cast<size_t>(taps[k])) v += gains[k] * far[i - static_cast<size_t>(taps[k])];
        mic[i] = static_cast<short>(v);
    }
    for (size_t i = 0; i < near.size(); ++i) mic[12000 + i] = static_cast<short>(mic[12000 + i] + near[i]);
    mbe_encoder_options opts;
    mbe_encoderDefaultOptions(&opts);
    opts.echo_cancel = 1;
    std::vector<short> out = frontEnd(mic, opts, &far);
    // echo return loss enhancement over 1.0..1.5 s (converged, far end only) and 2.6..3.0 s
    const double erle1 = rmsDb(&mic[8000], 4000) - rmsDb(&out[8000], 4000);
    const double erle2 = rmsDb(&mic[20800], 3000) - rmsDb(&out[20800], 3000);
    // the near end survives double talk: the output over 1.6..2.4 s correlates with it
    double num = 0, den1 = 0, den2 = 0;
    for (size_t i = 12800; i < 19200; ++i) { double a = out[i], b = near[i - 12000]; num += a * b; den1 += a * a; den2 += b * b; }
    const double corr = num / std::sqrt(den1 * den2 + 1e-9);
    std::printf("echo cancellation: ERLE %.1f dB (far end only), %.1f dB (after double talk), near-end correlation %.2f\n", erle1, erle2, corr);
    CHECK(erle1 > 15.0);
    CHECK(erle2 > 10.0);
    CHECK(corr > 0.8);
    // no reference given: nothing is removed
    opts.echo_cancel = 1;
    std::vector<short> untouched = frontEnd(mic, opts, nullptr);
    CHECK_EQ(std::memcmp(untouched.data(), mic.data(), untouched.size() * sizeof(short)), 0);
}

int main()
{
    noiseSuppression();
    echoCancellation();
    return test::finish("test_frontend");
}
