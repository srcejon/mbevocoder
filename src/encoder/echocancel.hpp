// SPDX-License-Identifier: GPL-3.0-or-later
// Acoustic echo cancellation for the encoder input (docs/frontend.md): an NLMS filter on
// the far-end reference the application played out, a Geigel double-talk detector that
// freezes adaptation while the near end talks, and a residual suppressor for what the filter
// leaves when only the far end is talking.
#pragma once

namespace mbe {

class EchoCanceller {
public:
    static constexpr int kFrame = 160;
    static constexpr int kTaps = 256;   // 32 ms of echo path

    EchoCanceller();
    void reset();
    // Remove from the 160 microphone samples `mic` the echo of the 160 far-end samples
    // `far` played out over the same 20 ms (NULL = silence was played). In place.
    void process(short mic[kFrame], const short* far);

private:
    float m_x[kTaps + kFrame];   // far-end history, newest last
    float m_w[kTaps];            // filter taps
    float m_xPower;              // far-end energy over the filter span
    int m_dtHang;                // double-talk hangover (frames)
    float m_micPow, m_errPow;    // smoothed powers for the residual suppressor
    float m_residGain;           // current residual suppression gain (ramped)
};

} // namespace mbe
