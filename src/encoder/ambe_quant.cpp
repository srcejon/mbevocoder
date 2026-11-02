// SPDX-License-Identifier: GPL-3.0-or-later
#include "internal/config.h"
#include "encoder/ambe_quant.hpp"
#include "encoder/dstar_analysis.hpp"
#include "encoder/dstar_quantizer.hpp"

#include "analysis/imbe_vocoder/imbe.h"

// The shared tables are written as double literals assigned to float.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4305)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-float-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#endif
#include "ambe3600x2400_const.h"
#include "ambe3600x2450_const.h"
#include "ambe_pred.h"
#if MBEVOCODER_CHIP_MEASURED
#include "ambe3600x2450_vuvmix.h"
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cmath>
#include <cstring>

namespace mbe {

namespace {

constexpr float kPi = 3.14159265358979f;
// Default floor for the integer analysis amplitudes (docs/encoder-quality.md).
constexpr float kSaFloor = 2.0f;
// AMBE+2 level calibration: 2.0 log2 from the per-harmonic regression against the AMBE-3000R
// (phase 3), plus 0.6 dB so that the AMBE-3000R decodes our frames at the level of its own encoding
// once the frames are aligned with the AMBE-3000R's (docs/encoder-quality.md).
constexpr float kLevel2450 = 2.0f - 0.6f / 6.0206f;
// D-STAR level calibration: with the memoryless gain in the closed loop the AMBE-3000R decodes
// our frames 0.2 dB quieter than its own encoding of the same speech (docs/decoder-quality.md;
// the earlier +2.6 dB was compensating the wrong gain dynamics), so the target is raised by
// that much on top of the AMBE+2 constant.
constexpr float kLevel2400 = 2.0f - 0.2f / 6.0206f;

#if MBEVOCODER_CHIP_MEASURED
int dstarHarmonicVoiced(int b1, float hz) { return AmbePlusHarmonicVoiced(b1, hz); }
#endif

float f0From2450Table(int b0)
{
#if MBEVOCODER_CHIP_MEASURED
    return AmbeW0Measured(b0);   // measured, see the decoder
#else
    return AmbeW0table[b0];
#endif
}
// D-STAR: the decoder derives the fundamental from a formula rather than a table.
float f0From2400Formula(int b0)
{
#if MBEVOCODER_CHIP_MEASURED
    return std::exp2(-4.258420f - 2.17690e-2f * static_cast<float>(b0));   // measured, see decoder
#else
    return std::exp2(-4.311767578125f - 2.1336e-2f * (static_cast<float>(b0) + 0.5f));
#endif
}

// The AMBE-3000R's silence frame (its DTX output), as indices in the field order in use: the
// same bits read as 87 / 77 in mbelib's order of b3 / b4's low bits (src/params/fields.cpp).
#if MBEVOCODER_CHIP_MEASURED
const int kSilence2450[9] = {119, 16, 0, 87, 78, 8, 14, 13, 7};
#else
const int kSilence2450[9] = {119, 16, 0, 87, 77, 8, 14, 13, 7};
#endif

template <int M>
int nearest(const float (*table)[M], const float* v, int count, int rows, int step)
{
    int best = 0;
    float bestErr = 0;
    for (int i = 0; i < rows; i += step) {
        float err = 0;
        for (int k = 0; k < count; ++k) { float d = v[k] - table[i][k]; err += d * d; }
        if (i == 0 || err < bestErr) { bestErr = err; best = i; }
    }
    return best;
}

} // namespace

const AmbeRateTables& ambe2450Tables()
{
    static const AmbeRateTables t = {
        120, f0From2450Table,
#if MBEVOCODER_CHIP_MEASURED
        AmbeLtableMeasured,
#else
        AmbeLtable,
#endif
        AmbeVuv, 17, AmbeDg, 32, AmbePRBA24, AmbePRBA58,
        {AmbeHOCb5, AmbeHOCb6, AmbeHOCb7, AmbeHOCb8}, {32, 16, 16, 8}, {1, 1, 1, 1},
        AmbeLmprbl, mbe_decodeAmbe2450Parms, mbe_packAmbe2450Fields, kSilence2450, kLevel2450, AMBE2450_PRED, AMBE2450_GAIN_PRED,
        {1.00f, 1.00f, 2.00f, 1.20f, 0.70f, 0.70f, 0.70f, 0.70f}, 5.0f,
        // silence frames: index 124 = 15 bands at w0 = 2 pi / 32, voicing index 16, the
        // AMBE-3000R renders them log2(1 / 0.2046) below the voice law and keeps its state
        124, 15, 1.0f / 32.0f, 16, 2.289f, false,
        50,   // unvoiced frames on at least 50 harmonics (measured, docs/encoder-quality.md)
#if MBEVOCODER_CHIP_MEASURED
        AmbeVuvMix, AmbeVuvGainDb, 32, (1u << 20) | (1u << 24) | (1u << 25) | (1u << 28), nullptr,
#else
        nullptr, nullptr, 0, 0u, nullptr,
#endif
    };
    return t;
}

const AmbeRateTables& ambe2400Tables()
{
    // b8 has three real bits: mbelib reads it as a 4-bit index with a zero LSB, so only the
    // even rows of the b8 table are reachable (hocStep 2).
    static const AmbeRateTables t = {
        120, f0From2400Formula,
#if MBEVOCODER_CHIP_MEASURED
        AmbePlusLtableMeasured,
#else
        AmbePlusLtable,
#endif
        AmbePlusVuv, 16, AmbePlusDg, 64, AmbePlusPRBA24, AmbePlusPRBA58,
        {AmbePlusHOCb5, AmbePlusHOCb6, AmbePlusHOCb7, AmbePlusHOCb8}, {16, 16, 16, 16}, {1, 1, 1, 2},
        AmbePlusLmprbl, mbe_decodeAmbe2400Parms, mbe_packAmbe2400Fields, nullptr, kLevel2400, AMBE2400_PRED, AMBE2400_GAIN_PRED,
        {0.70f, 1.20f, 1.50f, 1.00f, 0.85f, 0.70f, 0.70f, 0.85f}, 3.0f,
        // silence frames: index 120, one step below the lowest voice fundamental, an
        // ordinary frame to the decoder
        120,
#if MBEVOCODER_CHIP_MEASURED
        static_cast<int>(AmbePlusLtableMeasured[120]),
#else
        static_cast<int>(AmbePlusLtable[120]),
#endif
        f0From2400Formula(120), 0, 0.0f, true,
        0,    // the same rule costs 0.08 MOS on D-STAR: off
#if MBEVOCODER_CHIP_MEASURED
        nullptr, nullptr, 16, 0u,   // the decoder renders AmbePlusHarmonicVoiced, binary
        dstarHarmonicVoiced,
#else
        nullptr, nullptr, 0, 0u, nullptr,
#endif
    };
    return t;
}

AmbeQuantizer::AmbeQuantizer(const AmbeRateTables& t) : m_t(t) { reset(); }

void AmbeQuantizer::reset() { mbe_initMbeParms(&m_cur, &m_prev, &m_enh); m_silValid = false; }

void AmbeQuantizer::commit(const int b[9])
{
    m_silValid = false;   // a voice frame ends the run of silence frames
    char d[49];
    m_t.packFields(b, d);
    m_t.decodeParms(d, &m_cur, &m_prev);
    mbe_moveMbeParms(&m_cur, &m_prev);
}

// Voicing decision rules tuned against the AMBE-3000R's decisions on the corpus
// (tools/quality/tune_voicing.py, docs/encoder-quality.md). The analysis stage's IMBE
// thresholds are applied per AMBE band with these multipliers; when the four lowest bands
// are all voiced the upper bands get kVoicingBoost more (the AMBE-3000R codes fully voiced frames
// far more often); and a frame whose analysed fundamental is below kVoicingLowPitchHz that
// would voice only the lowest one or two bands is coded unvoiced (the pitch tracker settles
// at the bottom of its range on noise, and the lowest band then fits any harmonic comb).
// (multipliers and boost per rate in the rate tables: the AMBE-3000R's AMBE+2 and D-STAR encoders
// differ slightly; tuned with tools/quality/tune_voicing.py --rate ...)
const float kVoicingLowPitchHz = 95.0f;

bool AmbeQuantizer::quantize(const IMBE_PARAM& p, bool silent, float gainAdjust, float amplitudeFloor, const float voicingThreshold[8], int b[9],
                             const QuantizerOverride* ov, DstarAnalysis* fa)
{
    if (p.ref_pitch <= 0 || p.num_harms < 1) return false;

    // Digital silence: the AMBE-3000R emits a canonical frame (lowest fundamental, all bands
    // unvoiced, minimum gain step) so the decoder's gain decays to its floor. Match it.
    if (silent && m_t.silenceFrame) {
        std::memcpy(b, m_t.silenceFrame, 9 * sizeof(int));
        commit(b);
        return true;
    }

    // --- b0: nearest fundamental (log domain) to the analysed pitch; the decoder's harmonic
    // count L follows from the table and the analysis is resampled onto those harmonics.
    const float f0a = 256.0f / static_cast<float>(p.ref_pitch);   // cycles/sample (ref_pitch is Q8.8 samples)
    b[0] = nearestB0(f0a);
    int L = static_cast<int>(m_t.Ltable[b[0]]);
    float f0 = m_t.f0(b[0]);

    // Re-decide voicing per analysis band with the tuned per-AMBE-band thresholds; the
    // amplitude estimate follows the decision (voiced or unvoiced hypothesis).
    int decided[NUM_HARMS_MAX];
    float saDecided[NUM_HARMS_MAX];
    float mult[8];
    for (int k = 0; k < 8; ++k) mult[k] = voicingThreshold && voicingThreshold[k] > 0.0f ? voicingThreshold[k] : m_t.voicingMult[k];
    // are the four lowest AMBE bands voiced throughout under the base thresholds?
    bool lowAllVoiced = true;
    for (int i = 0; i < p.num_harms; ++i) {
        int ab = static_cast<int>(static_cast<float>(i + 1) * 16.0f * f0a);
        if (ab >= 4) break;
        int band = i / 3;
        if (band >= p.num_bands) band = p.num_bands - 1;
        const float thr = static_cast<float>(p.band_thr[band]) * mult[ab];
        if (!(thr > 0.0f && static_cast<float>(p.band_dk[band]) < thr)) { lowAllVoiced = false; break; }
    }
    for (int i = 0; i < p.num_harms; ++i) {
        int band = i / 3;
        if (band >= p.num_bands) band = p.num_bands - 1;
        // AMBE band of this harmonic at the analysed fundamental
        int ab = static_cast<int>(static_cast<float>(i + 1) * 16.0f * f0a);
        if (ab > 7) ab = 7;
        float m = mult[ab];
        if (ab >= 4 && lowAllVoiced) m *= m_t.voicingBoost;
        const float thr = static_cast<float>(p.band_thr[band]) * m;
        bool voiced = thr > 0.0f && static_cast<float>(p.band_dk[band]) < thr;
        decided[i] = voiced ? 1 : 0;
    }
    if (f0a * 8000.0f < kVoicingLowPitchHz) {
        // low-pitch rule: voiced harmonics only in the lowest two AMBE bands -> unvoiced frame
        bool any = false, onlyLow = true;
        for (int i = 0; i < p.num_harms; ++i) {
            if (!decided[i]) continue;
            any = true;
            if (static_cast<int>(static_cast<float>(i + 1) * 16.0f * f0a) >= 2) { onlyLow = false; break; }
        }
        if (any && onlyLow) for (int i = 0; i < p.num_harms; ++i) decided[i] = 0;
    }
    if (m_t.unvoicedMinL > 0 && L < m_t.unvoicedMinL) {
        // Unvoiced frames on a fine grid: the tracker's pitch in noise is arbitrary, and a
        // high one codes the noise spectrum in a few wide bands. The AMBE-3000R codes most of
        // its unvoiced AMBE+2 frames with 50-56 bands; doing the same (fully unvoiced frames
        // only) is worth 0.05-0.06 MOS through its decoder (docs/encoder-quality.md).
        bool anyVoiced = false;
        for (int i = 0; i < p.num_harms; ++i) if (decided[i]) { anyVoiced = true; break; }
        if (!anyVoiced) {
            for (int i = 0; i < m_t.b0Count; ++i) if (m_t.Ltable[i] >= static_cast<float>(m_t.unvoicedMinL)) { b[0] = i; break; }
            L = static_cast<int>(m_t.Ltable[b[0]]);
            f0 = m_t.f0(b[0]);
        }
    }
    if (ov && ov->b1 >= 0 && ov->b1 < m_t.vuvRows) {
        // oracle: the reference voicing pattern, read per harmonic at the analysed fundamental
        for (int i = 0; i < p.num_harms; ++i) {
            int ab = static_cast<int>(static_cast<float>(i + 1) * 16.0f * f0a);
            if (ab > 7) ab = 7;
            decided[i] = m_t.vuv[ov->b1][ab];
        }
    }
    for (int i = 0; i < p.num_harms; ++i) {
        const bool voiced = decided[i] != 0;
        // Keep the analysis stage's own estimate when the decision is unchanged (bit-for-bit
        // the previous behaviour), otherwise the other hypothesis' estimate.
        saDecided[i] = (voiced == (p.v_uv_dsn[i] != 0)) ? static_cast<float>(p.sa[i])
                                                          : static_cast<float>(voiced ? p.sa_v[i] : p.sa_u[i]);
    }

    float logSa[NUM_HARMS_MAX + 1];
    int vuv[NUM_HARMS_MAX + 1];
    {
        float aLog[NUM_HARMS_MAX];
        const float floorV = amplitudeFloor > 0.0f ? amplitudeFloor : kSaFloor;
        for (int i = 0; i < p.num_harms; ++i) {
            float sa = saDecided[i];
            aLog[i] = std::log2(sa < floorV ? floorV : sa);
        }
        const float ratio = f0 / f0a;
        for (int l = 1; l <= L; ++l) {
            float pos = static_cast<float>(l) * ratio;
            if (pos < 1.0f) pos = 1.0f;
            if (pos > static_cast<float>(p.num_harms)) pos = static_cast<float>(p.num_harms);
            int i0 = static_cast<int>(pos);
            int i1 = i0 < p.num_harms ? i0 + 1 : i0;
            float frac = pos - static_cast<float>(i0);
            logSa[l] = (1.0f - frac) * aLog[i0 - 1] + frac * aLog[i1 - 1];
            int near = static_cast<int>(pos + 0.5f);
            if (near < 1) near = 1;
            if (near > p.num_harms) near = p.num_harms;
            vuv[l] = decided[near - 1];
        }
    }

    // --- target log amplitudes in the decoder's domain (voiced: + 0.5 log2 L; unvoiced also
    // undo the decoder's unvoiced scale 0.2046/sqrt(w0)), minus the level calibration.
    const float Lf = static_cast<float>(L);
    const float logL2 = 0.5f * std::log2(Lf);
    const float logLw0 = 0.5f * std::log2(Lf * f0 * 2.0f * kPi) + 2.289f;
    float lsa[NUM_HARMS_MAX];
    for (int l = 1; l <= L; ++l)
        lsa[l - 1] = (vuv[l] ? logL2 : logLw0) + logSa[l] - m_t.levelCal;

    if (ov && ov->L > 0 && ov->w0 > 0.0f) {
        // oracle: the reference shape (mean-removed log amplitudes, resampled onto this grid
        // by harmonic position) replaces ours, keeping our gain
        float mean = 0.0f;
        for (int l = 1; l <= L; ++l) mean += lsa[l - 1];
        mean /= Lf;
        float refMean = 0.0f;
        for (int l = 1; l <= ov->L; ++l) refMean += ov->log2Ml[l];
        refMean /= static_cast<float>(ov->L);
        const float ratio = (f0 * 2.0f * kPi) / ov->w0;
        for (int l = 1; l <= L; ++l) {
            float pos = static_cast<float>(l) * ratio;
            if (pos < 1.0f) pos = 1.0f;
            if (pos > static_cast<float>(ov->L)) pos = static_cast<float>(ov->L);
            int i0 = static_cast<int>(pos);
            int i1 = i0 < ov->L ? i0 + 1 : i0;
            float frac = pos - static_cast<float>(i0);
            lsa[l - 1] = mean + (1.0f - frac) * ov->log2Ml[i0] + frac * ov->log2Ml[i1] - refMean;
        }
    }
    if (ov && ov->haveGamma) {
        // oracle: the reference gain replaces ours (shape kept)
        float mean = 0.0f;
        for (int l = 1; l <= L; ++l) mean += lsa[l - 1];
        mean /= Lf;
        for (int l = 1; l <= L; ++l) lsa[l - 1] += ov->gamma - mean;
    }

    // oracle runs only (encoderForceModel): a voicing-only override keeps the floating-point
    // analysis, so the oracle changes nothing but the voicing
    const bool voicingOnly = ov && ov->b1 >= 0 && !ov->haveGamma && ov->L == 0;
    const bool shapeOnly = ov && ov->b1 < 0 && !ov->haveGamma && ov->L > 0;   // oracle runs: the reference shape on unmoved frames
    if (fa && (!ov || voicingOnly || shapeOnly)) {
        // floating-point analysis (dstar_analysis.hpp): the voicing index as chosen here, then
        // the fundamental index and spectral target it gives; the mean target, and so the gain,
        // is kept.
        const int b1 = chooseVoicingRow(vuv, logSa, L, f0, nullptr);
        float target[NUM_HARMS_MAX];
        const int b0 = fa->frame(m_t, b[0], b1, L, lsa, target);
        if (shapeOnly && b0 == b[0])
            for (int l = 0; l < L; ++l) target[l] = lsa[l];
        quantizeTail(b0, vuv, logSa, target, gainAdjust, b, 0, 0.0f, b1, true, nullptr, true);
        return true;
    }
    quantizeTail(b[0], vuv, logSa, lsa, gainAdjust, b, 0, 0.0f, -1, true, nullptr, true);
    return true;
}

bool AmbeQuantizer::quantizeModel(float w0, int L, const int* Vl, const float* Ml, float gainAdjust, int b[9], const float* Vmix)
{
    if (!(w0 > 0.0f) || L < 1 || L > NUM_HARMS_MAX) return false;
    const float f0s = w0 / (2.0f * kPi);
    b[0] = nearestB0(f0s);
    const int Lt = static_cast<int>(m_t.Ltable[b[0]]);
    const float f0 = m_t.f0(b[0]);
    // Resample the source harmonics (log amplitude, nearest voicing) onto the target grid.
    float srcLog[NUM_HARMS_MAX + 1];
    for (int l = 1; l <= L; ++l) srcLog[l] = std::log2(Ml[l] > 1e-6f ? Ml[l] : 1e-6f);
    float logSa[NUM_HARMS_MAX + 1], lsa[NUM_HARMS_MAX], want[NUM_HARMS_MAX + 1];
    int vuv[NUM_HARMS_MAX + 1];
    const float ratio = f0 / f0s;
    const float logUnvc = std::log2(0.2046f / std::sqrt(f0 * 2.0f * kPi));
    // The gain the decoders reconstruct is mean(log2Ml) + 0.5 log2 L, and quantizeTail's
    // targets live in that domain (see quantize), so add the half log harmonic count.
    const float logLt2 = 0.5f * std::log2(static_cast<float>(Lt));
    for (int l = 1; l <= Lt; ++l) {
        float pos = static_cast<float>(l) * ratio;
        if (pos < 1.0f) pos = 1.0f;
        if (pos > static_cast<float>(L)) pos = static_cast<float>(L);
        int i0 = static_cast<int>(pos);
        int i1 = i0 < L ? i0 + 1 : i0;
        float frac = pos - static_cast<float>(i0);
        logSa[l] = (1.0f - frac) * srcLog[i0] + frac * srcLog[i1];
        int near = static_cast<int>(pos + 0.5f);
        if (near < 1) near = 1;
        if (near > L) near = L;
        vuv[l] = Vl[near] ? 1 : 0;
        want[l] = Vmix ? Vmix[near] : (vuv[l] ? 1.0f : 0.0f);
        // Decoder-domain target: the decoders scale unvoiced amplitudes by 0.2046/sqrt(w0)
        // after the log domain, so undo that for the unvoiced harmonics.
        lsa[l - 1] = logSa[l] + logLt2 - (vuv[l] ? 0.0f : logUnvc);
    }
    quantizeTail(b[0], vuv, logSa, lsa, gainAdjust, b, 0, 0.0f, -1, true, Vmix ? want : nullptr);
    return true;
}

bool AmbeQuantizer::quantizeSilence(const IMBE_PARAM& p, float gainAdjust, float amplitudeFloor, int b[9])
{
    if (p.ref_pitch <= 0 || p.num_harms < 1) return false;
    const float f0a = 256.0f / static_cast<float>(p.ref_pitch);
    const int L = m_t.silenceL;
    const float f0 = m_t.silenceF0;
    // the unvoiced amplitude estimate of every analysis harmonic, resampled onto the grid
    float aLog[NUM_HARMS_MAX];
    const float floorV = amplitudeFloor > 0.0f ? amplitudeFloor : kSaFloor;
    for (int i = 0; i < p.num_harms; ++i) {
        float sa = static_cast<float>(p.v_uv_dsn[i] ? p.sa_u[i] : p.sa[i]);
        aLog[i] = std::log2(sa < floorV ? floorV : sa);
    }
    float logSa[NUM_HARMS_MAX + 1], lsa[NUM_HARMS_MAX];
    int vuv[NUM_HARMS_MAX + 1];
    const float ratio = f0 / f0a;
    const float Lf = static_cast<float>(L);
    const float logLw0 = 0.5f * std::log2(Lf * f0 * 2.0f * kPi) + 2.289f + m_t.silenceLevel;
    for (int l = 1; l <= L; ++l) {
        float pos = static_cast<float>(l) * ratio;
        if (pos < 1.0f) pos = 1.0f;
        if (pos > static_cast<float>(p.num_harms)) pos = static_cast<float>(p.num_harms);
        int i0 = static_cast<int>(pos);
        int i1 = i0 < p.num_harms ? i0 + 1 : i0;
        float frac = pos - static_cast<float>(i0);
        logSa[l] = (1.0f - frac) * aLog[i0 - 1] + frac * aLog[i1 - 1];
        // the background over the run of silence frames rather than this 20 ms of it: with
        // DTX the application sends one silence frame in several, each standing alone
        if (m_silValid) logSa[l] = 0.8f * m_silLog[l] + 0.2f * logSa[l];
        m_silLog[l] = logSa[l];
        vuv[l] = 0;
        lsa[l - 1] = logLw0 + logSa[l] - m_t.levelCal;
    }
    quantizeTail(m_t.silenceB0, vuv, logSa, lsa, gainAdjust, b, L, f0, m_t.silenceB1, m_t.silenceAdvances);
    m_silValid = true;   // (commit clears it on the rates whose silence frames advance the state)
    return true;
}

int AmbeQuantizer::nearestB0(float f0) const
{
    const float logF0 = std::log2(f0);
    int best = 0; float bestErr = 0;
    for (int i = 0; i < m_t.b0Count; ++i) {
        float d = std::fabs(std::log2(m_t.f0(i)) - logF0);
        if (i == 0 || d < bestErr) { bestErr = d; best = i; }
    }
    return best;
}

int AmbeQuantizer::chooseVoicingRow(const int* vuv, const float* logSa, int L, float f0, const float* wantFrac) const
{
    float enMin = 0;
    int row = 0;
    const bool byFraction = wantFrac != nullptr && m_t.mixFraction != nullptr;
    const int rows = byFraction ? m_t.mixRows : m_t.vuvRows;
    for (int n = 0; n < rows; ++n) {
        if (byFraction && n < 32 && ((m_t.mixTransientRows >> n) & 1u)) continue;
        float en = 0;
        for (int l = 1; l <= L; ++l) {
            int jl = static_cast<int>(static_cast<float>(l) * 16.0f * f0);
            if (m_t.harmonicVoiced) {
                const int want = wantFrac ? (wantFrac[l] >= 0.5f) : vuv[l];
                if (want != m_t.harmonicVoiced(n, static_cast<float>(l) * f0 * 8000.0f)) en += std::exp2(2.0f * logSa[l]);
            } else if (byFraction) {
                const float d = m_t.mixFraction[n][jl] - wantFrac[l];
                en += d * d * std::exp2(2.0f * logSa[l]);
            } else if (vuv[l] != m_t.vuv[n][jl]) {
                en += std::exp2(2.0f * logSa[l]);
            }
        }
        if (n == 0) enMin = en;
        else if (en < enMin) { row = n; enMin = en; }
    }
    return row;
}

void AmbeQuantizer::quantizeTail(int b0, const int* vuv, const float* logSa, const float* lsaIn, float gainAdjust, int b[9],
                                 int L, float f0, int forceB1, bool advance, const float* wantFrac, bool refineShape)
{
    if (L <= 0) { L = static_cast<int>(m_t.Ltable[b0]); f0 = m_t.f0(b0); }
    const float Lf = static_cast<float>(L);
    b[0] = b0;

    // --- b1: voicing pattern minimising the energy of mis-voiced harmonics. For analysis
    // decisions (wantFrac null) the binary table is the measure: weighting the rows by the
    // decoder's measured voiced fractions, and rebuilding the amplitude targets for the row
    // the decoder renders, were tried on the encoder and cost 0.005 and 0.02 MOS through the
    // AMBE-3000R's decoder (docs/encoder-quality.md). For a decoded model (the transcoder;
    // wantFrac = the source's fractions) the rows' measured renderings are matched, every
    // row of the table included except the transient ones, and the chosen row's level
    // offsets are taken out of the targets: that is what lets a decoded frame be
    // re-quantised to itself (docs/encoder-quality.md, "Mixed frames and the voicing index").
    b[1] = forceB1 >= 0 ? forceB1 : chooseVoicingRow(vuv, logSa, L, f0, wantFrac);
    const bool byFraction = wantFrac != nullptr && m_t.mixFraction != nullptr;
    float lsaRow[NUM_HARMS_MAX];
    std::memcpy(lsaRow, lsaIn, static_cast<size_t>(L) * sizeof(float));
    if (byFraction && m_t.mixGainDb) {
        for (int l = 1; l <= L; ++l) {
            int jl = static_cast<int>(static_cast<float>(l) * 16.0f * f0);
            lsaRow[l - 1] -= m_t.mixGainDb[b[1]][jl] / 6.0206f;
        }
    }
    const float* lsa = lsaRow;

    // --- b2: gain step from the mean target log amplitude.
    float lsaSum = 0.0f;
    for (int l = 0; l < L; ++l) lsaSum += lsa[l];
    float gain = lsaSum / Lf;
    // The decoder forms gamma = 2 (1 - c) dg[b2] + c gamma_prev (c = gainPred), so quantise
    // the part the table has to supply.
    float diffGain = (gain - m_t.gainPred * m_prev.gamma - gainAdjust) / (2.0f * (1.0f - m_t.gainPred));
    {
        int best = 0; float bestErr = 0;
        for (int i = 0; i < m_t.dgCount; ++i) {
            float d = std::fabs(diffGain - m_t.dg[i]);
            if (i == 0 || d < bestErr) { bestErr = d; best = i; }
        }
        b[2] = best;
    }

    // --- prediction residuals against the previous decoded log amplitudes.
    const float lPrevL = static_cast<float>(m_prev.L) / Lf;
    m_prev.log2Ml[0] = m_prev.log2Ml[1];
    float T[NUM_HARMS_MAX];
    for (int i = 0; i < L; ++i) {
        float kl = lPrevL * static_cast<float>(i + 1);
        int klf = static_cast<int>(kl);
        if (klf > NUM_HARMS_MAX) klf = NUM_HARMS_MAX;
        float frac = kl - static_cast<float>(klf);
        // the upper neighbour is clamped as the decoders clamp it (kl reaches L_prev exactly on
        // the top harmonic, and log2Ml has 57 entries)
        const int upper = klf < NUM_HARMS_MAX ? klf + 1 : NUM_HARMS_MAX;
        T[i] = lsa[i] - m_t.pred * (1.0f - frac) * m_prev.log2Ml[klf] - m_t.pred * frac * m_prev.log2Ml[upper];
    }

    // --- DCT of each of the four blocks.
    const int* J = m_t.lmprbl[L];
    const float* c[4];
    int acc = 0;
    for (int i = 0; i < 4; ++i) { c[i] = &T[acc]; acc += J[i]; }
    float C[4][17];
    for (int i = 0; i < 4; ++i) {
        for (int k = 1; k <= J[i]; ++k) {
            float s = 0;
            for (int j = 1; j <= J[i]; ++j)
                s += c[i][j - 1] * std::cos((kPi * (static_cast<float>(k) - 1.0f) * (static_cast<float>(j) - 0.5f)) / static_cast<float>(J[i]));
            C[i][k - 1] = s / static_cast<float>(J[i]);
        }
    }
    const float sqrt2 = std::sqrt(2.0f);
    float R[8];
    for (int i = 0; i < 4; ++i) {
        R[2 * i] = C[i][0] + sqrt2 * C[i][1];
        R[2 * i + 1] = C[i][0] - sqrt2 * C[i][1];
    }

    // --- PRBA (b3, b4)
    float G[8];
    for (int m = 1; m <= 8; ++m) {
        G[m - 1] = 0;
        for (int i = 1; i <= 8; ++i)
            G[m - 1] += R[i - 1] * std::cos((kPi * (static_cast<float>(m) - 1.0f) * (static_cast<float>(i) - 0.5f)) / 8.0f);
        G[m - 1] /= 8.0f;
    }
    b[3] = nearest(m_t.prba24, &G[1], 3, 512, 1);
    b[4] = nearest(m_t.prba58, &G[4], 4, 128, 1);

    // --- higher order coefficients (b5..b8)
    for (int blk = 0; blk < 4; ++blk) {
        if (J[blk] <= 2) { b[5 + blk] = 0; continue; }
        int count = J[blk] - 2;
        if (count > 4) count = 4;
        b[5 + blk] = nearest(m_t.hoc[blk], &C[blk][2], count, m_t.hocRows[blk], m_t.hocStep[blk]);
    }

#if MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER
    if (refineShape && &m_t == &ambe2400Tables()) refineDstarShape(m_t, L, T, lsa, b);
#else
    (void)refineShape;
#endif
    if (advance) commit(b);
}

} // namespace mbe
