// SPDX-License-Identifier: GPL-3.0-or-later
// Tone detection modelled on the AMBE-3000R (measured with tools/quality/probe_tone_detect.py,
// docs/frontend.md): the decision is per frame with no hold-in or hold-off, a tone is
// declared from about -37 dBFS up and down to about 17 dB of tone-to-noise ratio, an
// off-grid frequency goes to the nearest tone id, DTMF is accepted up to 10 dB of twist, and
// the amplitude id is 127 - round((0.2 - L) / 0.714) with L the component level in dBFS
// (the mean of the two levels for dual tones).
#include "tonedetect.hpp"

#include <mbevocoder/mbevocoder.h>

#include <cmath>
#include <cstring>

namespace mbe {

namespace {

constexpr int kN = 256;
constexpr float kPi = 3.14159265358979f;
constexpr float kMinLevelDb = -37.0f;    // below this the AMBE-3000R does not send tone frames
constexpr float kMinSnrDb = 17.0f;       // tone-to-residual ratio the AMBE-3000R needs
constexpr float kDualMaxTwistDb = 10.5f;
constexpr float kHarmonicRejectDb = 25.0f; // a second or third harmonic closer than this to the peak means voice
constexpr float kDualMatchHz = 36.0f;    // sum of both errors; the AMBE-3000R took 820+1305 Hz as 820+1279

// In-place radix-2 complex FFT, 256 points.
void fft256(float* re, float* im)
{
    for (int i = 1, j = 0; i < kN; ++i) {
        int bit = kN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= kN; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < kN; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

struct Peak { float hz; float db; };

// Least-squares fit of sinusoids at the given frequencies to the frame; returns the
// residual energy fraction and the fitted amplitudes.
float fitSines(const float* x, int n, const float* hz, int count, float* amp)
{
    // Gram matrix of [cos, sin] pairs (count <= 2, so 4x4 at most)
    float basis[4][160];
    for (int c = 0; c < count; ++c) {
        const float w = 2.0f * kPi * hz[c] / 8000.0f;
        for (int i = 0; i < n; ++i) { basis[2 * c][i] = std::cos(w * static_cast<float>(i)); basis[2 * c + 1][i] = std::sin(w * static_cast<float>(i)); }
    }
    const int m = 2 * count;
    float G[4][4], r[4];
    for (int a = 0; a < m; ++a) {
        r[a] = 0.0f;
        for (int i = 0; i < n; ++i) r[a] += basis[a][i] * x[i];
        for (int b = 0; b < m; ++b) { float s = 0.0f; for (int i = 0; i < n; ++i) s += basis[a][i] * basis[b][i]; G[a][b] = s; }
    }
    // solve G c = r by Gaussian elimination
    float c[4] = {0, 0, 0, 0};
    for (int col = 0; col < m; ++col) {
        int piv = col;
        for (int row = col + 1; row < m; ++row) if (std::fabs(G[row][col]) > std::fabs(G[piv][col])) piv = row;
        if (std::fabs(G[piv][col]) < 1e-9f) { for (int k = 0; k < count; ++k) amp[k] = 0.0f; return 1.0f; }
        if (piv != col) { for (int k = 0; k < m; ++k) { float t = G[col][k]; G[col][k] = G[piv][k]; G[piv][k] = t; } float t = r[col]; r[col] = r[piv]; r[piv] = t; }
        for (int row = col + 1; row < m; ++row) {
            const float f = G[row][col] / G[col][col];
            for (int k = col; k < m; ++k) G[row][k] -= f * G[col][k];
            r[row] -= f * r[col];
        }
    }
    for (int row = m - 1; row >= 0; --row) {
        float s = r[row];
        for (int k = row + 1; k < m; ++k) s -= G[row][k] * c[k];
        c[row] = s / G[row][row];
    }
    float etot = 0.0f, eres = 0.0f;
    for (int i = 0; i < n; ++i) {
        float y = 0.0f;
        for (int a = 0; a < m; ++a) y += c[a] * basis[a][i];
        etot += x[i] * x[i];
        eres += (x[i] - y) * (x[i] - y);
    }
    for (int k = 0; k < count; ++k) amp[k] = std::sqrt(c[2 * k] * c[2 * k] + c[2 * k + 1] * c[2 * k + 1]);
    return etot > 0.0f ? eres / etot : 1.0f;
}

int amplitudeId(float meanDb)
{
    const int ad = 127 - static_cast<int>(std::lround((0.2f - meanDb) / 0.714f));
    return ad < 0 ? 0 : (ad > 127 ? 127 : ad);
}

float dbfs(float amplitude) { return 20.0f * std::log10(amplitude / 32767.0f + 1e-12f); }

} // namespace

ToneDetector::ToneDetector() { reset(); }

void ToneDetector::reset() { std::memset(m_prev, 0, sizeof m_prev); }

int ToneDetector::detect(const short pcm[160], int* toneId, int* amplitude)
{
    float x[160];
    for (int i = 0; i < 160; ++i) x[i] = static_cast<float>(pcm[i]);
    // 256-point analysis over the previous 96 samples and this frame, Hann window
    float re[kN], im[kN];
    for (int i = 0; i < kN; ++i) {
        const float v = i < 96 ? m_prev[i] : x[i - 96];
        const float w = 0.5f - 0.5f * std::cos(2.0f * kPi * (static_cast<float>(i) + 0.5f) / kN);
        re[i] = v * w; im[i] = 0.0f;
    }
    std::memcpy(m_prev, x + 64, sizeof m_prev);
    // quick level gate on the frame itself
    float peak = 0.0f;
    for (int i = 0; i < 160; ++i) peak = std::fabs(x[i]) > peak ? std::fabs(x[i]) : peak;
    if (dbfs(peak) < kMinLevelDb - 3.0f) return 0;

    fft256(re, im);
    float mag[kN / 2 + 1];
    for (int k = 0; k <= kN / 2; ++k) mag[k] = 10.0f * std::log10(re[k] * re[k] + im[k] * im[k] + 1e-6f);

    // the two strongest local maxima between 100 and 3900 Hz, with parabolic interpolation
    Peak best[2] = {{0.0f, -1e9f}, {0.0f, -1e9f}};
    for (int k = 4; k < kN / 2 - 3; ++k) {
        if (mag[k] <= mag[k - 1] || mag[k] < mag[k + 1]) continue;
        const float d = 0.5f * (mag[k - 1] - mag[k + 1]) / (mag[k - 1] - 2.0f * mag[k] + mag[k + 1] + 1e-9f);
        const float hz = (static_cast<float>(k) + d) * 8000.0f / kN;
        if (hz < 100.0f || hz > 3900.0f) continue;
        if (mag[k] > best[0].db) { best[1] = best[0]; best[0] = {hz, mag[k]}; }
        else if (mag[k] > best[1].db) best[1] = {hz, mag[k]};
    }
    if (best[0].db <= -1e8f) return 0;

    // dual tone: table entries whose two frequencies sit near the two strongest peaks, or
    // (the call-progress pairs, 40 to 140 Hz apart, which the 31 Hz analysis does not resolve)
    // whose mean sits near the strongest peak; each candidate is fitted and the best kept
    {
        int bestId = -1;
        float bestResid = 1.0f, bestL1 = 0.0f, bestL2 = 0.0f;
        const float threshold = std::pow(10.0f, -kMinSnrDb / 10.0f);
        const bool twoPeaks = best[1].db > best[0].db - kDualMaxTwistDb - 3.0f;
        for (int cand = 128; cand <= 163; ++cand) {
            float f1, f2;
            mbe_toneFrequencies(cand, &f1, &f2);
            bool near = false;
            if (twoPeaks) {
                const float e1 = std::fabs(f1 - best[0].hz) + std::fabs(f2 - best[1].hz);
                const float e2 = std::fabs(f1 - best[1].hz) + std::fabs(f2 - best[0].hz);
                near = (e1 < e2 ? e1 : e2) < kDualMatchHz;
            }
            bool merged = std::fabs(f1 - f2) < 150.0f;   // close pairs: the 31 Hz analysis cannot place both peaks
            if (!near && merged) near = std::fabs(0.5f * (f1 + f2) - best[0].hz) < 120.0f;
            if (!near) continue;
            // the purity test fits the measured peak frequencies (the AMBE-3000R accepts a pair some
            // 26 Hz off its nominal one); the nominal pair is used when the peaks merged
            float amp[2];
            float hz[2] = {f1, f2};
            if (!merged) {
                const bool swap = std::fabs(f1 - best[1].hz) + std::fabs(f2 - best[0].hz) < std::fabs(f1 - best[0].hz) + std::fabs(f2 - best[1].hz);
                hz[0] = swap ? best[1].hz : best[0].hz;
                hz[1] = swap ? best[0].hz : best[1].hz;
            }
            const float resid = fitSines(x, 160, hz, 2, amp);
            if (resid < bestResid) { bestResid = resid; bestId = cand; bestL1 = dbfs(amp[0]); bestL2 = dbfs(amp[1]); }
        }
        if (bestId >= 0 && bestResid < threshold && std::fabs(bestL1 - bestL2) <= kDualMaxTwistDb
            && 0.5f * (bestL1 + bestL2) >= kMinLevelDb - 6.0f) {
            *toneId = bestId;
            *amplitude = amplitudeId(0.5f * (bestL1 + bestL2));
            return 1;
        }
    }
    // single tone: nearest tone id to the strongest peak, fitted at the peak's own frequency;
    // a low-pitched vowel can be as pure as a tone within 20 ms, so its harmonics at 2f and 3f
    // must also be absent (the AMBE-3000R codes no tone frames on the speech corpus)
    {
        float amp[1];
        const float hz[1] = {best[0].hz};
        const float resid = fitSines(x, 160, hz, 1, amp);
        const float level = dbfs(amp[0]);
        bool harmonics = false;
        for (int h = 2; h <= 3; ++h) {
            const float fh = best[0].hz * static_cast<float>(h);
            if (fh > 3900.0f) break;
            const int k = static_cast<int>(fh * kN / 8000.0f + 0.5f);
            float m = mag[k];
            if (mag[k - 1] > m) m = mag[k - 1];
            if (mag[k + 1] > m) m = mag[k + 1];
            if (m > best[0].db - kHarmonicRejectDb) harmonics = true;
        }
        if (!harmonics && resid < std::pow(10.0f, -kMinSnrDb / 10.0f) && level >= kMinLevelDb) {
            const int id = mbe_toneIdFromFrequencies(best[0].hz, 0.0f);
            if (id < 0) return 0;
            *toneId = id;
            *amplitude = amplitudeId(level);
            return 1;
        }
    }
    return 0;
}

} // namespace mbe
