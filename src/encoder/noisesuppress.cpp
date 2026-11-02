// SPDX-License-Identifier: GPL-3.0-or-later
// Noise suppression (docs/frontend.md). Analysis and synthesis with a square-root Hann
// window of 320 samples every 160 (perfect reconstruction at unity gain), 512-point FFT.
// The noise power per bin is the minimum of the smoothed periodogram over the last two
// seconds (Martin's minimum statistics, eight sub-windows), times a bias compensation. The
// gain is a Wiener filter on the decision-directed a priori SNR (Ephraim and Malah), floored
// at -18 dB and smoothed over three bins against musical noise. Tuned by PESQ against the
// clean speech (tools/quality/probe_ns.py): the AMBE-3000R's suppressor, which takes 12 to
// 15 dB out of stationary noise in pauses when it acts at all, was not a usable reference.
#include "noisesuppress.hpp"

#include "fft.hpp"

#include <cmath>
#include <cstring>

namespace mbe {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kSmooth = 0.7f;          // periodogram smoothing
constexpr float kBias = 1.2f;            // minimum statistics bias compensation
constexpr float kDd = 0.95f;             // decision-directed weight on the previous estimate
constexpr float kGainFloorDb = -18.0f;   // tuned by PESQ on speech in white and pink noise at 5..25 dB SNR
constexpr float kSnrMax = 1000.0f;

float window(int i) { return std::sqrt(0.5f - 0.5f * std::cos(2.0f * kPi * (static_cast<float>(i) + 0.5f) / NoiseSuppressor::kWindow)); }

} // namespace

NoiseSuppressor::NoiseSuppressor() { reset(); }

void NoiseSuppressor::reset()
{
    std::memset(m_input, 0, sizeof m_input);
    std::memset(m_overlap, 0, sizeof m_overlap);
    for (int k = 0; k < kBins; ++k) {
        m_smooth[k] = 0.0f;
        m_curMin[k] = 1e30f;
        m_noise[k] = 0.0f;
        m_gain[k] = 1.0f;
        m_postSnr[k] = 1.0f;
        for (int s = 0; s < kSubWindows; ++s) m_subMin[s][k] = 1e30f;
    }
    m_subCount = 0;
    m_subIndex = 0;
    m_frames = 0;
    m_sinceZero = 0;
}

void NoiseSuppressor::process(const short in[kFrame], short out[kFrame])
{
    // slide the input window
    std::memmove(m_input, m_input + kFrame, (kWindow - kFrame) * sizeof(float));
    for (int i = 0; i < kFrame; ++i) m_input[kWindow - kFrame + i] = static_cast<float>(in[i]);

    float re[kFft], im[kFft];
    for (int i = 0; i < kWindow; ++i) { re[i] = m_input[i] * window(i); im[i] = 0.0f; }
    for (int i = kWindow; i < kFft; ++i) { re[i] = 0.0f; im[i] = 0.0f; }
    fftRadix2(re, im, kFft);

    const float floorGain = std::pow(10.0f, kGainFloorDb / 20.0f);
    float gain[kBins];
    if (m_frames < 2) ++m_frames;   // only the first frame matters; bounded
    // digital silence says nothing about the background: it and the smoothing ramp out of
    // it are left out of the minimum, and the smoothed periodogram restarts after it
    bool zeroFrame = true;
    for (int i = 0; i < kFrame && zeroFrame; ++i) if (in[i] != 0) zeroFrame = false;
    const bool restart = m_frames == 1 || (m_sinceZero == 0 && !zeroFrame);
    m_sinceZero = zeroFrame ? 0 : (m_sinceZero < (1 << 20) ? m_sinceZero + 1 : m_sinceZero);   // bounded
    const bool zero = zeroFrame || m_sinceZero < 4;
    for (int k = 0; k < kBins; ++k) {
        const float p = re[k] * re[k] + im[k] * im[k];
        // smoothed periodogram and its minimum over the sub-windows
        m_smooth[k] = restart ? p : kSmooth * m_smooth[k] + (1.0f - kSmooth) * p;
        if (!zero && m_smooth[k] < m_curMin[k]) m_curMin[k] = m_smooth[k];
        float mn = m_curMin[k];
        for (int s = 0; s < m_subIndex; ++s) if (m_subMin[s][k] < mn) mn = m_subMin[s][k];
        m_noise[k] = kBias * mn + 1e-3f;
        // decision-directed a priori SNR from the last frame's gain and posterior SNR
        const float post = p / m_noise[k];
        float prio = kDd * m_gain[k] * m_gain[k] * m_postSnr[k] + (1.0f - kDd) * (post > 1.0f ? post - 1.0f : 0.0f);
        if (prio > kSnrMax) prio = kSnrMax;
        float g = prio / (1.0f + prio);
        if (g < floorGain) g = floorGain;
        gain[k] = g;
        m_postSnr[k] = post;
    }
    // close the sub-window every kSubLength frames
    if (!zero && ++m_subCount >= kSubLength) {
        m_subCount = 0;
        for (int s = kSubWindows - 1; s > 0; --s) std::memcpy(m_subMin[s], m_subMin[s - 1], sizeof m_subMin[s]);
        std::memcpy(m_subMin[0], m_curMin, sizeof m_curMin);
        if (m_subIndex < kSubWindows) ++m_subIndex;
        for (int k = 0; k < kBins; ++k) m_curMin[k] = 1e30f;
    }
    // smooth the gain over three bins, keep it for the next frame's a priori estimate
    float smoothed[kBins];
    for (int k = 0; k < kBins; ++k) {
        const float a = gain[k > 0 ? k - 1 : 0], b = gain[k], c = gain[k < kBins - 1 ? k + 1 : k];
        smoothed[k] = 0.25f * a + 0.5f * b + 0.25f * c;
    }
    std::memcpy(m_gain, smoothed, sizeof m_gain);
    // apply, inverse transform, window and overlap-add
    for (int k = 0; k < kBins; ++k) {
        re[k] *= smoothed[k]; im[k] *= smoothed[k];
        if (k > 0 && k < kBins - 1) { re[kFft - k] = re[k]; im[kFft - k] = -im[k]; }
    }
    ifftRadix2(re, im, kFft);
    float y[kWindow];
    for (int i = 0; i < kWindow; ++i) y[i] = re[i] / static_cast<float>(kFft) * window(i);
    for (int i = 0; i < kFrame; ++i) {
        float v = y[i] + m_overlap[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = static_cast<short>(std::lround(v));
    }
    std::memcpy(m_overlap, y + kFrame, (kWindow - kFrame) * sizeof(float));
}

} // namespace mbe
