// SPDX-License-Identifier: GPL-3.0-or-later
// AMBE parameter quantiser shared by 3600x2450 (AMBE+2) and 3600x2400 (D-STAR): MBE model
// parameters from the IMBE analysis -> the nine quantiser indices b[0..8], with closed-loop
// prediction state so the encoder tracks exactly what the library's decoder reconstructs.
// Structure ported from OP25's ambe_encoder.cc (Max H. Parke KA1RBI, GPL-3.0-or-later);
// see docs/encoder-quality.md for the changes made against the AMBE-3000R.
#pragma once

#include <mbevocoder/mbevocoder.h>

struct IMBE_PARAM;
constexpr int NUM_HARMS_MAX_OVERRIDE = 56;

namespace mbe {

// Everything that differs between the two AMBE rates.
struct AmbeRateTables {
    int b0Count;                          // number of fundamental indices
    float (*f0)(int b0);                  // cycles/sample for an index
    const float* Ltable;                  // harmonic count per index
    const int (*vuv)[8];                  // voicing patterns
    int vuvRows;                          // usable rows
    const float* dg;                      // gain steps
    int dgCount;
    const float (*prba24)[3];
    const float (*prba58)[4];
    const float (*hoc[4])[4];             // b5..b8 tables
    int hocRows[4];
    int hocStep[4];                       // index step (2 for D-STAR b8, whose LSB is unused)
    const int (*lmprbl)[4];               // block lengths per L
    int (*decodeParms)(const char* d, mbe_parms* cur, mbe_parms* prev);
    int (*packFields)(const int b[9], char d[49]);
    const int* silenceFrame;              // canonical digital-silence indices, or null
    float levelCal;                       // log2 offset from analysis scale to decoder scale
    float pred;                           // decoder's spectral amplitude prediction coefficient
    float gainPred;                       // decoder's gain prediction coefficient (gamma = 2(1-c) dg + c prev)
    float voicingMult[8];                 // per AMBE band multipliers on the analysis voicing threshold
    float voicingBoost;                   // extra multiplier on bands 4..7 when bands 0..3 are all voiced
    // DTX silence frames as the AMBE-3000R codes them (docs/frontend.md): the fundamental
    // index, the harmonic count and fundamental (cycles/sample) the decoder gives it, the
    // all-unvoiced voicing index, an extra log2 level the decoder takes off, and whether
    // the decoder's prediction state advances over it.
    int silenceB0;
    int silenceL;
    float silenceF0;
    int silenceB1;
    float silenceLevel;
    bool silenceAdvances;
    // Fully unvoiced frames are coded on a grid of at least this many harmonics (0 = the
    // tracker's pitch as it is); the AMBE-3000R's AMBE+2 encoder does, D-STAR's does not.
    int unvoicedMinL;
    // The decoder's rendering of each voicing index and band (MBEVOCODER_CHIP_MEASURED): voiced
    // energy fraction and level offset in dB; null when the decoder renders the binary table.
    const float (*mixFraction)[8];
    const float (*mixGainDb)[8];
    int mixRows;                          // rows of those tables (every value of b1)
    // Rows the AMBE-3000R renders as something other than a steady excitation (AMBE+2 20,
    // 24, 25, 28: a noise sequence restarted every frame, a buzz at the frame rate;
    // docs/decoder-quality.md "Unvoiced rows 18-31: the band grid"), never chosen for a
    // model that does not mean that; bit n = row n.
    unsigned mixTransientRows;
    // whether voicing row b1 voices a harmonic at hz, when the decoder renders a rule rather than
    // vuv (D-STAR with MBEVOCODER_CHIP_MEASURED: AmbePlusHarmonicVoiced); null otherwise
    int (*harmonicVoiced)(int b1, float hz);
};

const AmbeRateTables& ambe2450Tables();
const AmbeRateTables& ambe2400Tables();

// Per-frame overrides for oracle experiments (tools/quality/frontend_oracle.py): each part
// of the analysis can be replaced by a reference value to bound what a better estimate of it
// could gain. Unused parts: b1 < 0, haveGamma false, L 0 (voicedL 0 for the IMBE voicing).
struct QuantizerOverride {
    int b1 = -1;                          // voicing pattern index (this rate's table)
    float gamma = 0.0f;                   // decoder-domain gain target (mean log2Ml + 0.5 log2 L)
    bool haveGamma = false;
    int L = 0;                            // reference harmonic count for log2Ml
    float w0 = 0.0f;                      // reference fundamental (radians/sample) of log2Ml
    float log2Ml[NUM_HARMS_MAX_OVERRIDE + 1] = {0};   // reference log amplitudes [1..L], mean-removed shape is used
    // IMBE only: the reference's per-harmonic voicing [1..voicedL] on its own grid (w0), used
    // when b1 >= 0; the gain override is then the reference's mean log2Ml
    int voicedL = 0;
    float voicedW0 = 0.0f;                // the voicing's fundamental (radians/sample); 0: w0
    unsigned char voiced[NUM_HARMS_MAX_OVERRIDE + 1] = {0};
};

class DstarAnalysis;

class AmbeQuantizer {
public:
    explicit AmbeQuantizer(const AmbeRateTables& t);
    void reset();
    // Quantise one frame. silent marks an all-zero input frame. gainAdjust is subtracted
    // from the log2 gain on top of the built-in calibration; amplitudeFloor (0 = default)
    // floors the integer analysis amplitudes. Returns false if the pitch is out of range.
    // voicingThreshold: per AMBE band multipliers on the analysis stage's voicing threshold
    // (0 = library default for that band); harmonics are re-decided with them.
    // fa (optional): the D-STAR floating-point analysis of the same stream, which may move the
    // fundamental index and replaces the spectral target (MBEVOCODER_DSTAR_PITCH_ANALYSIS).
    bool quantize(const IMBE_PARAM& p, bool silent, float gainAdjust, float amplitudeFloor, const float voicingThreshold[8], int b[9],
                  const QuantizerOverride* ov = nullptr, DstarAnalysis* fa = nullptr);
    // Quantise one frame from MBE model parameters in the decoders' domain (transcoding):
    // w0 in radians/sample, L harmonics, Vl[1..L] voicing and Ml[1..L] linear amplitudes as
    // mbe_decode*Parms produce them. Returns false if w0 is outside the rate's range.
    // Vmix (optional): the source's voiced energy fraction per harmonic; the target voicing
    // index is then the one whose rendering is nearest, and the target's level offsets are
    // taken out of the amplitude targets so the rendered levels are the source's (this is
    // what makes re-quantising a decoded frame reproduce it).
    bool quantizeModel(float w0, int L, const int* Vl, const float* Ml, float gainAdjust, int b[9], const float* Vmix = nullptr);
    // Quantise one frame as a DTX silence frame describing the background: the rate's
    // silence fundamental index, all bands unvoiced, the analysis amplitudes (unvoiced
    // estimate) on that grid. The AMBE-3000R's AMBE+2 decoder renders a silence frame from the last
    // voice frame's state without updating it, so the codes are formed against that state
    // and, for that rate, the state is left as it is.
    bool quantizeSilence(const IMBE_PARAM& p, float gainAdjust, float amplitudeFloor, int b[9]);
    const mbe_parms& current() const { return m_cur; }
    const AmbeRateTables& tables() const { return m_t; }

private:
    int nearestB0(float f0) const;
    // Common tail: b1..b8 from the target-grid voicing vuv[1..L], analysis-domain log
    // amplitudes logSa[1..L] (energy weighting only) and decoder-domain targets lsa[0..L-1].
    // wantFrac (optional): the voiced fraction wanted per target harmonic [1..L], used with
    // the decoder's measured row renderings to choose b1 and to compensate the row's level
    // offsets in the targets.
    void quantizeTail(int b0, const int* vuv, const float* logSa, const float* lsa, float gainAdjust, int b[9],
                      int L = 0, float f0 = 0.0f, int forceB1 = -1, bool advance = true, const float* wantFrac = nullptr,
                      bool refineShape = false);
    void commit(const int b[9]);
    // The voicing index quantizeTail chooses for these decisions (see there).
    int chooseVoicingRow(const int* vuv, const float* logSa, int L, float f0, const float* wantFrac) const;
    const AmbeRateTables& m_t;
    mbe_parms m_cur{}, m_prev{}, m_enh{};
    float m_silLog[NUM_HARMS_MAX_OVERRIDE + 1] = {0};   // smoothed background log amplitudes over a run of silence frames
    bool m_silValid = false;
};

} // namespace mbe
