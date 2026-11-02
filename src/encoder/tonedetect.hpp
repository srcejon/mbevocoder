// SPDX-License-Identifier: GPL-3.0-or-later
// Automatic tone detection for the encoder, modelled on the AMBE-3000R's (docs/frontend.md):
// per frame, a single tone at any tone-ID frequency, a DTMF digit, a KNOX tone or a
// call-progress tone is recognised and coded as a tone frame instead of voice.
#pragma once

namespace mbe {

class ToneDetector {
public:
    ToneDetector();
    void reset();
    // Classify one 20 ms frame. Returns 1 and fills the canonical tone id (5..122 single,
    // 128..163 dual) and the amplitude id (0..127) for a tone, 0 for voice.
    int detect(const short pcm[160], int* toneId, int* amplitude);

private:
    float m_prev[96];   // tail of the previous frame: the analysis window is 256 samples
};

} // namespace mbe
