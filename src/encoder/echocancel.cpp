// SPDX-License-Identifier: GPL-3.0-or-later
// Echo cancellation (docs/frontend.md). Sample by sample: e = d - w.x over the last 256
// far-end samples, NLMS update w += mu e x / (|x|^2 + delta) unless the double-talk detector
// holds it. Geigel's test: the near end is talking when the microphone sample exceeds half
// the largest far-end sample of the filter span, held for 40 ms. The residual suppressor
// attenuates the error by up to 12 dB when the far end is active and the filter is taking
// at least 6 dB out, i.e. when what remains is residual echo rather than near-end speech.
#include "echocancel.hpp"

#include <cmath>
#include <cstring>

namespace mbe {

namespace {

constexpr float kMu = 0.5f;
constexpr float kDelta = 1e3f * 160.0f;       // regularisation (far-end below about -50 dBFS adapts slowly)
constexpr float kGeigel = 0.5f;               // near-end above half the far-end peak = double talk
constexpr int kDtHangFrames = 2;
constexpr float kFarActive = 100.0f;          // far-end peak above this (about -50 dBFS) counts as active
constexpr float kResidualGain = 0.25f;        // -12 dB
constexpr float kErleForSuppress = 4.0f;      // 6 dB of cancellation before the residual is suppressed

} // namespace

EchoCanceller::EchoCanceller() { reset(); }

void EchoCanceller::reset()
{
    std::memset(m_x, 0, sizeof m_x);
    std::memset(m_w, 0, sizeof m_w);
    m_xPower = 0.0f;
    m_dtHang = 0;
    m_micPow = m_errPow = 0.0f;
    m_residGain = 1.0f;
}

void EchoCanceller::process(short mic[kFrame], const short* far)
{
    // append the far-end frame to the history
    std::memmove(m_x, m_x + kFrame, kTaps * sizeof(float));
    for (int i = 0; i < kFrame; ++i) m_x[kTaps + i] = far ? static_cast<float>(far[i]) : 0.0f;
    // recompute the span energy and peak at the frame start (then track incrementally)
    float power = 0.0f, peak = 0.0f;
    for (int i = 0; i < kTaps; ++i) { power += m_x[i] * m_x[i]; if (std::fabs(m_x[i]) > peak) peak = std::fabs(m_x[i]); }
    m_xPower = power;
    bool doubleTalk = false;
    float micPow = 0.0f, errPow = 0.0f;
    float out[kFrame];
    for (int n = 0; n < kFrame; ++n) {
        const float* x = m_x + n + 1;   // x[kTaps-1] is the newest far-end sample for this mic sample
        // slide the span: drop x[n], add x[n + kTaps]
        m_xPower += m_x[n + kTaps] * m_x[n + kTaps] - m_x[n] * m_x[n];
        if (m_xPower < 0.0f) m_xPower = 0.0f;
        if (std::fabs(m_x[n + kTaps]) > peak) peak = std::fabs(m_x[n + kTaps]);
        float y = 0.0f;
        for (int k = 0; k < kTaps; ++k) y += m_w[k] * x[k];
        const float d = static_cast<float>(mic[n]);
        const float e = d - y;
        out[n] = e;
        micPow += d * d; errPow += e * e;
        // Geigel double-talk detector
        if (std::fabs(d) > kGeigel * peak && peak > kFarActive) doubleTalk = true;
        if (!doubleTalk && m_dtHang == 0 && peak > kFarActive) {
            const float step = kMu * e / (m_xPower + kDelta);
            for (int k = 0; k < kTaps; ++k) m_w[k] += step * x[k];
        }
    }
    if (doubleTalk) m_dtHang = kDtHangFrames;
    else if (m_dtHang > 0) --m_dtHang;
    // residual echo suppression: far end active, no double talk, the filter is working
    m_micPow = 0.5f * m_micPow + 0.5f * micPow;
    m_errPow = 0.5f * m_errPow + 0.5f * errPow;
    const bool suppress = peak > kFarActive && m_dtHang == 0 && !doubleTalk && m_errPow * kErleForSuppress < m_micPow;
    const float target = suppress ? kResidualGain : 1.0f;
    for (int n = 0; n < kFrame; ++n) {
        m_residGain += (target - m_residGain) * (1.0f / 64.0f);   // about 8 ms ramp
        float v = out[n] * m_residGain;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        mic[n] = static_cast<short>(std::lround(v));
    }
}

} // namespace mbe
