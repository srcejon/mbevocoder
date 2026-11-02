// SPDX-License-Identifier: GPL-3.0-or-later
// D-STAR floating-point analysis (MBEVOCODER_DSTAR_PITCH_ANALYSIS, docs/encoder-quality.md): a
// streaming port of the offline candidate of 22-23 September (tools/quality/dstar_pitch.py).
// For each voice frame it
//  - checks the quantiser's pitch index against the normalised correlation of the low-passed
//    input at all 120 transmitted pitch periods and, on voiced frames where another period is
//    clearly more periodic and continuous with the previous frame, moves the index;
//  - replaces the spectral target with harmonic band energies measured on the input with a
//    2048-point transform on the (possibly new) harmonic grid, keeping the frame's mean target
//    (its gain).
// The voicing index is the quantiser's own. No look-ahead beyond the encoder's: the windows end
// 249 samples before the newest input sample. Double precision throughout, as the reference.
#pragma once

#include "ambe_quant.hpp"

#include <cstdint>

namespace mbe {

class DstarAnalysis {
public:
    DstarAnalysis();
    void reset();
    // The encoder's input alignment in samples (98 for D-STAR, 48 for AMBE+2 and IMBE): the
    // analysis windows sit at the same place relative to the frame whatever the rate.
    void setFrameAlign(int samples) { m_align = samples; }
    // Append one 20 ms block of the encoder's input (after the front end, before its delay line).
    void push(const short pcm[160]);
    // For the frame just pushed: the quantiser chose fundamental index b0 and voicing index b1
    // with decoder-domain targets lsa[0..L-1]. Returns the fundamental index to code and fills
    // target[0..L'-1] for it (L' = the table's harmonic count of that index).
    int frame(const AmbeRateTables& t, int b0, int b1, int L, const float* lsa, float target[56]);
    // The same for any set of candidate fundamentals: w0s[i] (radians per sample) with Ls[i]
    // harmonics, the caller's choice b0 among them, whether the frame has voiced bands, and its
    // log2 amplitude targets lsa[0..L-1] (any scale: only their mean is kept). Returns the index
    // to code; target[0..Ls[index]-1] receives log2 amplitudes on the same scale. A null target
    // asks for the pitch alone and skips the band-energy transform (lsa is then not read).
    int select(const double* w0s, const int* Ls, int count, int b0, bool voiced, int L, const float* lsa, float* target);
    static constexpr int kMaxCandidates = 256;

    static constexpr int kHistory = 1024;      // samples of input and low-passed history kept

    // Tests: move the stream's sample count on by `samples` (a multiple of kHistory, so the
    // history buffers stay aligned and the analysis is unchanged), to run past 2^31 samples.
    void advanceSampleCountForTest(int64_t samples);

private:
    double signal(int64_t n) const;               // DC-blocked input at stream sample n (0 before the stream)
    double low(int64_t n) const;                  // its 1200 Hz low-pass (centred 129-tap FIR)
    void energyAmplitudes(int64_t start, double w0, int L, double amp[56]) const;
    void lowWindow(double* win) const;                   // 2 * 256 + 1 low-passed samples around the centre
    static double correlationIn(const double* win, double w0);

    double m_signal[kHistory];
    double m_low[kHistory];
    // 64-bit: a `long` is 32 bits on Windows and 32-bit ARM, and wrapped negative after 2^31
    // samples (74.6 hours of one stream), indexing before the buffers.
    int64_t m_pushed;                          // input samples received
    int64_t m_lowDone;                         // low() is valid below this sample
    double m_dcX, m_dcY;                       // DC blocker state
    int m_last;                                // pitch index chosen for the previous voice frame, -1 before the first
    int m_align = 98;
};

} // namespace mbe
