// SPDX-License-Identifier: GPL-3.0-or-later
// Single-channel noise suppression for the encoder input (docs/frontend.md): a weighted
// overlap-add spectral gain with a minimum-statistics noise estimate and decision-directed
// a priori SNR, one frame (160 samples) of delay.
#pragma once

namespace mbe {

class NoiseSuppressor {
public:
    static constexpr int kFrame = 160;
    static constexpr int kWindow = 320;
    static constexpr int kFft = 512;
    static constexpr int kBins = kFft / 2 + 1;

    NoiseSuppressor();
    void reset();
    // Consume 160 input samples and produce the 160 output samples that end one frame
    // earlier (the suppressor's delay). `in` and `out` may alias.
    void process(const short in[kFrame], short out[kFrame]);

private:
    static constexpr int kSubWindows = 8;     // minimum statistics: 8 sub-windows of 12 frames (about 2 s)
    static constexpr int kSubLength = 12;

    float m_input[kWindow];                   // the last 320 input samples
    float m_overlap[kWindow - kFrame];        // synthesis overlap-add tail
    float m_smooth[kBins];                    // smoothed periodogram
    float m_subMin[kSubWindows][kBins];       // minimum of each sub-window
    float m_curMin[kBins];                    // running minimum of the current sub-window
    float m_noise[kBins];                     // noise power estimate
    float m_gain[kBins];                      // last gain (decision-directed SNR)
    float m_postSnr[kBins];                   // last a posteriori SNR
    int m_subCount;                           // frames in the current sub-window
    int m_subIndex;                           // sub-windows filled so far (saturates)
    int m_frames;
    int m_sinceZero;                          // frames since the last all-zero input frame
};

} // namespace mbe
