// SPDX-License-Identifier: GPL-3.0-or-later
// Voice activity detection for the encoder's DTX mode, fitted to the AMBE-3000R's decisions
// (tools/quality/probe_dtx.py, docs/frontend.md): frame energy against a slowly rising,
// instantly falling background estimate, an absolute threshold above which everything is
// voice, and a short hangover.
#pragma once

namespace mbe {

class Vad {
public:
    Vad();
    void reset();
    // Classify one 20 ms frame (the frame the encoder is about to quantise). Returns 1 for
    // voice, 0 for background.
    int process(const short pcm[160]);

private:
    float m_floor;      // background mean-square estimate, full scale = 1
    int m_hang;         // hangover frames left
    bool m_init;        // floor initialised from the first non-zero frame
};

} // namespace mbe
