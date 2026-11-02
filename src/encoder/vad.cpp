// SPDX-License-Identifier: GPL-3.0-or-later
// The AMBE-3000R's VAD (docs/frontend.md): the decision follows the frame energy relative to a
// background estimate. The estimate falls to the frame energy at once, is dropped altogether
// by a near-silent frame (below -45 dBFS: the AMBE-3000R re-learns the background from
// nothing after any quiet stretch, not only digital silence), and rises by at most a factor
// per frame (about 0.4 dB), so after a step up in the background, or a quiet stretch, it
// takes about three seconds for pauses to be classed as silence again, as measured; white
// noise above about -35 dBFS is always voice; and voice holds for two frames after the last
// active one. Fitted to the AMBE-3000R's decisions on four speakers (96 % frame agreement).
#include "vad.hpp"

#include <cmath>

namespace mbe {

namespace {

constexpr float kAlpha = 0.05f;         // background estimate time constant (20 frames)
constexpr float kRiseLimit = 3.0f;      // the update input is capped at this multiple of the estimate
constexpr float kSnrDb = 8.0f;          // voice when the frame is this far above the background
constexpr float kAbsoluteDb = -35.0f;   // and always above this level (mean square, full scale = 1)
constexpr float kResetDb = -45.0f;      // a frame below this drops the background estimate to nothing
constexpr int kHangover = 2;
constexpr float kFloorMin = 1e-12f;

} // namespace

Vad::Vad() { reset(); }

void Vad::reset()
{
    m_floor = kFloorMin;
    m_hang = 0;
    m_init = false;
}

int Vad::process(const short pcm[160])
{
    double acc = 0.0;
    for (int i = 0; i < 160; ++i) acc += static_cast<double>(pcm[i]) * pcm[i];
    const float e = static_cast<float>(acc / (160.0 * 32767.0 * 32767.0));
    if (e <= 0.0f) {
        // digital silence: never voice, and it says nothing about the background
        if (m_hang > 0) { --m_hang; return 1; }
        return 0;
    }
    if (!m_init) { m_floor = e; m_init = true; }
    const float snr = std::pow(10.0f, kSnrDb / 10.0f);
    const float absolute = std::pow(10.0f, kAbsoluteDb / 10.0f);
    const float resetBelow = std::pow(10.0f, kResetDb / 10.0f);
    const bool voice = e > m_floor * snr || e > absolute;
    // background: first-order update towards the frame energy, the rise limited, the fall immediate
    float u = e < kRiseLimit * m_floor ? e : kRiseLimit * m_floor;
    m_floor += kAlpha * (u - m_floor);
    if (e < m_floor) m_floor = e;
    if (e < resetBelow) m_floor = kFloorMin;
    if (m_floor < kFloorMin) m_floor = kFloorMin;
    if (voice) { m_hang = kHangover; return 1; }
    if (m_hang > 0) { --m_hang; return 1; }
    return 0;
}

} // namespace mbe
