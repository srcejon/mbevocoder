// SPDX-License-Identifier: GPL-3.0-or-later
// Public encoder API: one mbe_encoder per stream, IMBE analysis front end shared by
// every rate, rate-specific quantisers behind it.
#include <mbevocoder/mbevocoder.h>

#include "analysis/imbe_vocoder/imbe_vocoder.h"
#include "encoder/ambe_quant.hpp"
#include "encoder/dstar_analysis.hpp"
#include "encoder/encoder_internal.hpp"
#include "encoder/echocancel.hpp"
#include "encoder/noisesuppress.hpp"
#include "encoder/tonedetect.hpp"
#include "encoder/vad.hpp"
#include "internal/bits.hpp"

#include <cmath>
#include <cstring>
#include <new>

// Frame timing: the AMBE-3000R's AMBE+2 frames cover audio 48 samples (6 ms) later than the
// analysis stage's natural framing, its D-STAR frames 98 samples later. Delaying the input by
// that much makes our frames describe the same 20 ms as the AMBE-3000R's, which halves the
// per-frame gain and shape differences against it (docs/encoder-quality.md; D-STAR measured
// separately, docs/encoder-quality.md) and keeps bridged streams in step.
constexpr int kFrameAlign2450 = 48;
constexpr int kFrameAlign2400 = 98;
constexpr int kFrameAlignMax = kFrameAlign2400;
constexpr int kAnalysisDelay = 141;

#ifndef MBEVOCODER_AMBE2450_PITCH_ANALYSIS
#define MBEVOCODER_AMBE2450_PITCH_ANALYSIS 0
#endif
#ifndef MBEVOCODER_IMBE_PITCH_ANALYSIS
#define MBEVOCODER_IMBE_PITCH_ANALYSIS 0
#endif
#ifndef MBEVOCODER_IMBE_VOICING_THRESHOLD
#define MBEVOCODER_IMBE_VOICING_THRESHOLD 0
#endif
// IMBE: the standard's voicing threshold scaled on every band, percent (docs/encoder-quality.md
// "Voicing threshold"); the AMBE rates re-decide voicing with their own multipliers instead.
constexpr int16_t kImbeVoicingScale = MBEVOCODER_IMBE_VOICING_THRESHOLD ? 175 : 100;

// Input gain in front of the fixed-point analysis (docs/encoder-quality.md "Quiet input"): it
// loses pitch and voicing on quiet speech (dam at -20 dB: half the AMBE+2 voicing indices
// change), so its input is raised by 2^k, block floating point: k from the peak of everything
// the analysis can still see, its stored history rescaled when k changes (the window holds one
// gain throughout), and the quantisers take k off again in the log2 domain, where it is exact.
#ifndef MBEVOCODER_ENCODER_INPUT_GAIN
#define MBEVOCODER_ENCODER_INPUT_GAIN 1
#endif
#ifndef MBE_EXP_INPUT_TARGET
#define MBE_EXP_INPUT_TARGET 16384   // the envelope is raised to at most this (-6 dBFS)
#endif
#ifndef MBE_EXP_INPUT_MAXSHIFT
#define MBE_EXP_INPUT_MAXSHIFT 8     // at most 48 dB
#endif
constexpr int kInputPeakFrames = 5;   // the analysis' history (621 samples) and this frame
// With the input gain the analysis also gates voicing on the frame's level above 200 Hz against
// its tracked maximum: no voicing 35 dB down, full voicing from 25 dB down (imbe_vocoder.h
// set_level_gate; docs/encoder-quality.md "Quiet input").
constexpr double kVoicingGateDb = -35.0;
constexpr double kVoicingGateWidthDb = 10.0;

// The background-noise gate on the voicing (imbe_vocoder.h set_noise_gate).
#ifndef MBE_EXP_NOISE_GATE
#define MBE_EXP_NOISE_GATE 1
#endif

// The floating-point analysis (dstar_analysis.hpp) on any rate (docs/encoder-quality.md).
#define MBE_FLOAT_ANALYSIS_ANY (MBEVOCODER_DSTAR_PITCH_ANALYSIS || MBEVOCODER_AMBE2450_PITCH_ANALYSIS || MBEVOCODER_IMBE_PITCH_ANALYSIS)

struct mbe_encoder {
    explicit mbe_encoder(mbe_rate r, const mbe::AmbeRateTables& tables) : rate(r), opts{}, ambe(tables) {}
    mbe_rate rate;
    mbe_encoder_options opts;
    imbe_vocoder analysis;
    mbe::AmbeQuantizer ambe;
    int16_t frameVector[8] = {0};   // IMBE codewords u0..u7 from the analysis stage
    float limiterEnv = 0.0f;  // peak envelope of the optional input limiter
    int frameAlign = kFrameAlign2450;     // input delay (samples) so frames cover the same 20 ms as the AMBE-3000R's
    short delayLine[kFrameAlignMax] = {0};
    short delayLineRaw[kFrameAlignMax] = {0};   // the same without the input gain (the VAD's input)
    int inputPeaks[kInputPeakFrames] = {0};   // raw peak of the last frames, newest first
    int inputShift = 0;                   // input gain 2^inputShift in front of the analysis
    int analysisShift = 0;                // the shift the last analysis output carries
    mbe::QuantizerOverride override;      // oracle experiments (encoder_internal.hpp)
    bool haveOverride = false;
    bool pitchForced = false;             // oracle experiments: encoderForcePitch set a pitch for the next frame
    mbe::ToneDetector toneDetector;
    short toneDelay[kAnalysisDelay + kFrameAlignMax] = {0};   // the detector sees the input with the voice frames' delay
    int frameFlags = 0;                   // MBE_ENCODER_FLAG_* of the last frame
    mbe::Vad vad;
    mbe::EchoCanceller echo;
    short farEnd[160] = {0};
    bool haveFarEnd = false;              // a reference was given for the next frame
    mbe::NoiseSuppressor noise;
    short noiseRawDelay[160] = {0};       // the suppressor delays the voice path a frame: the tone detector's raw input too
    short frontEndOut[160] = {0};         // the last frame after the front end (encoderFrontEndOutput)
#if MBE_FLOAT_ANALYSIS_ANY
    mbe::DstarAnalysis dstarAnalysis;     // floating-point analysis of the same input (dstar_analysis.hpp)
#endif
};

namespace {

bool isAmbe2450(mbe_rate r) { return r == MBE_RATE_AMBE3600x2450 || r == MBE_RATE_AMBE2450; }
bool isAmbe2400(mbe_rate r) { return r == MBE_RATE_AMBE3600x2400; }
bool isImbe(mbe_rate r) { return r == MBE_RATE_IMBE7200x4400 || r == MBE_RATE_IMBE4400; }
bool supported(mbe_rate r) { return isAmbe2450(r) || isAmbe2400(r) || isImbe(r); }
const mbe::AmbeRateTables& tablesFor(mbe_rate r) { return isAmbe2400(r) ? mbe::ambe2400Tables() : mbe::ambe2450Tables(); }

#if MBE_FLOAT_ANALYSIS_ANY
// Rates whose frames go through the floating-point analysis (dstar_analysis.hpp).
bool usesFloatAnalysis(mbe_rate r)
{
    return (MBEVOCODER_DSTAR_PITCH_ANALYSIS && isAmbe2400(r)) || (MBEVOCODER_AMBE2450_PITCH_ANALYSIS && isAmbe2450(r))
        || (MBEVOCODER_IMBE_PITCH_ANALYSIS && isImbe(r));
}
#endif

// IMBE codewords u0..u7 (12,12,12,12,11,11,11,7 bits) -> imbe_d[88] in mbelib order.
void imbeVectorToData(const int16_t u[8], char d[88])
{
    static const int widths[8] = {12, 12, 12, 12, 11, 11, 11, 7};
    int k = 0;
    for (int i = 0; i < 8; ++i)
        for (int bit = widths[i] - 1; bit >= 0; --bit) d[k++] = static_cast<char>((u[i] >> bit) & 1);
}

void toShort(const float* in, short* out)
{
    for (int i = 0; i < 160; ++i) {
        float v = in[i];
        if (!std::isfinite(v)) v = 0.0f;   // a NaN would pass both clamps into an undefined cast
        if (v > 32767.f) v = 32767.f;
        if (v < -32768.f) v = -32768.f;
        out[i] = static_cast<short>(v < 0 ? v - 0.5f : v + 0.5f);
    }
}

// Optional input limiter (mbe_encoder_options::limiter): instant attack on the sample
// envelope, 300 ms release, ceiling -1 dBFS. The analysis itself tracks level linearly up to
// full scale (docs/encoder-quality.md), so this only matters for float input above full
// scale, which would otherwise be hard clipped by the conversion to 16 bits.
void limitPeaks(mbe_encoder* e, float x[160])
{
    const float ceiling = 32767.0f * 0.891f;   // -1 dBFS
    const float release = 0.99958f;            // exp(-1 / (0.3 s * 8000))
    for (int i = 0; i < 160; ++i) {
        float a = std::fabs(x[i]);
        e->limiterEnv = a > e->limiterEnv ? a : e->limiterEnv * release;
        if (e->limiterEnv > ceiling) x[i] *= ceiling / e->limiterEnv;
    }
}

#if MBEVOCODER_IMBE_PITCH_ANALYSIS
// Oracle experiments (encoder_internal.hpp): parts of a reference model in place of the
// analysis' own, resampled onto the frame's harmonics by frequency; amplitudes on the
// analysis scale sa = 4 Ml (Q14.2).
void imbeApplyOverride(IMBE_PARAM* p, const mbe::QuantizerOverride& ov)
{
    constexpr float kLevel = 2.0f;   // sa is Q14.2: four times the decoder's amplitude
    const int L = p->num_harms;
    // the fundamental the decoder renders this frame at (the transmitted one, b0 from ref_pitch),
    // so a reference on the same grid maps harmonic l to l
    const int b0 = (p->ref_pitch - 0x1380) >> 7;
    const float w0 = 4.0f * 3.14159265f / (static_cast<float>(b0) + 39.5f);
    auto nearestRef = [&](int l, int count, float w0ref) {
        int n = static_cast<int>(static_cast<float>(l) * w0 / w0ref + 0.5f);
        return n < 1 ? 1 : n > count ? count : n;
    };
    const float vw0 = ov.voicedW0 > 0.0f ? ov.voicedW0 : ov.w0;   // the voicing's own grid
    if (ov.b1 >= 0 && ov.voicedL > 0 && vw0 > 0.0f) {
        for (int l = 1; l <= L; ++l) {
            const int v = ov.voiced[nearestRef(l, ov.voicedL, vw0)] ? 1 : 0;
            if (v != (p->v_uv_dsn[l - 1] ? 1 : 0)) p->sa[l - 1] = v ? p->sa_v[l - 1] : p->sa_u[l - 1];
            p->v_uv_dsn[l - 1] = static_cast<Word16>(v);
        }
    }
    if (!(ov.L > 0 && ov.w0 > 0.0f) && !ov.haveGamma) return;   // amplitudes untouched: no round trip through the log domain
    float la[NUM_HARMS_MAX];
    float mean = 0.0f;
    for (int l = 0; l < L; ++l) { la[l] = std::log2(static_cast<float>(p->sa[l] > 1 ? p->sa[l] : 1)) - kLevel; mean += la[l]; }
    mean /= static_cast<float>(L);
    if (ov.L > 0 && ov.w0 > 0.0f) {   // reference shape, own mean
        float ref[NUM_HARMS_MAX], rmean = 0.0f;
        for (int l = 1; l <= L; ++l) {
            float pos = static_cast<float>(l) * w0 / ov.w0;
            if (pos < 1.0f) pos = 1.0f;
            if (pos > static_cast<float>(ov.L)) pos = static_cast<float>(ov.L);
            const int i0 = static_cast<int>(pos), i1 = i0 < ov.L ? i0 + 1 : i0;
            const float frac = pos - static_cast<float>(i0);
            ref[l - 1] = (1.0f - frac) * ov.log2Ml[i0] + frac * ov.log2Ml[i1];
            rmean += ref[l - 1];
        }
        rmean /= static_cast<float>(L);
        for (int l = 0; l < L; ++l) la[l] = ref[l] - rmean + mean;
    }
    if (ov.haveGamma) {   // the reference's mean log2Ml
        for (int l = 0; l < L; ++l) la[l] += ov.gamma - mean;
    }
    for (int l = 0; l < L; ++l) {
        float sa = std::exp2(la[l] + kLevel);
        sa = sa > 32767.0f ? 32767.0f : sa;   // may round to 0, as the analysis' own amplitudes do
        p->sa[l] = static_cast<Word16>(sa + 0.5f);
    }
}

// IMBE through the floating-point analysis: the vocoder's pitch is checked against the input's
// periodicity at every transmitted period (b0 0..207, period (b0 + 39.5) / 2); when it moves,
// voicing and amplitudes are re-analysed at the new pitch from the state before that stage.
// The vocoder's own amplitudes are kept: band energies in their place lose 0.03 PESQ on IMBE
// (docs/encoder-quality.md).
void imbeFloatAnalysis(mbe_encoder* e, short buf[160], const mbe::QuantizerOverride* ov)
{
    constexpr int kCount = 208;
    struct Grid {
        double w0[kCount];
        int L[kCount];
        Grid()
        {
            for (int i = 0; i < kCount; ++i) {
                w0[i] = 4.0 * 3.141592653589793 / (i + 39.5);
                L[i] = imbe_vocoder::imbe_num_harms(static_cast<int16_t>((i + 39) * 128 + 64));
            }
        }
    };
    static const Grid grid;
    imbe_vocoder& v = e->analysis;
    v.imbe_analyse_pitch(buf);
    const imbe_vocoder before = v;
    v.imbe_analyse_voicing();
    const IMBE_PARAM* p = v.mutable_param();
    int b0 = (p->ref_pitch - 0x1380) >> 7;
    b0 = b0 < 0 ? 0 : b0 >= kCount ? kCount - 1 : b0;
    bool voiced = false;
    for (int l = 0; l < p->num_harms; ++l) voiced = voiced || p->v_uv_dsn[l] != 0;
    // a pitch forced by an oracle (encoderForcePitch) is not moved; IMBE keeps the vocoder's own
    // amplitudes, so the stage computes the pitch alone (no target)
    const int tracked = e->pitchForced ? b0 : e->dstarAnalysis.select(grid.w0, grid.L, kCount, b0, voiced, p->num_harms, nullptr, nullptr);
    e->pitchForced = false;
    if (tracked != b0) {
        v = before;
        imbe_vocoder::imbe_set_pitch(v.mutable_param(), static_cast<int16_t>((tracked + 39) * 128 + 64));
        v.imbe_analyse_voicing();
    }
    if (ov) {
        imbeApplyOverride(v.mutable_param(), *ov);
        v.imbe_encode_model(v.mutable_param(), e->frameVector);   // rebuilds the band voicing word
        return;
    }
    v.imbe_quantise(e->frameVector);
}
#endif

// Run the analysis on one frame and produce parameter bits for the encoder's rate. The
// input is already limited if the limiter is on (each entry point limits exactly once).
int analyseAndQuantize(mbe_encoder* e, const short pcmIn[160], char* d, int* b)
{
    // Front end (docs/frontend.md): echo cancellation against the far-end reference, then
    // noise suppression (one frame of delay, which the tone detector's raw input follows).
    short pcm[160], toneRaw[160];
    // An oracle override (encoder_internal.hpp) applies to this frame only, whatever it becomes
    // (a tone or silence frame used to leave it set for a later voice frame).
    const mbe::QuantizerOverride* ov = e->haveOverride ? &e->override : nullptr;
    e->haveOverride = false;
    std::memcpy(pcm, pcmIn, sizeof pcm);
    if (e->opts.echo_cancel) {
        e->echo.process(pcm, e->haveFarEnd ? e->farEnd : nullptr);
        e->haveFarEnd = false;
    }
    std::memcpy(toneRaw, pcm, sizeof toneRaw);
    if (e->opts.noise_suppress) {
        std::memcpy(toneRaw, e->noiseRawDelay, sizeof toneRaw);
        std::memcpy(e->noiseRawDelay, pcm, sizeof toneRaw);
        e->noise.process(pcm, pcm);
    }
    std::memcpy(e->frontEndOut, pcm, sizeof e->frontEndOut);
    short raw[160];
    std::memcpy(raw, pcm, sizeof raw);
#if MBEVOCODER_ENCODER_INPUT_GAIN
    {
        int peak = 0;
        for (short v : pcm) { const int a = v < 0 ? -v : v; peak = a > peak ? a : peak; }
        std::memmove(e->inputPeaks + 1, e->inputPeaks, (kInputPeakFrames - 1) * sizeof(int));
        e->inputPeaks[0] = peak;
        int recent = 0;
        for (int p : e->inputPeaks) recent = p > recent ? p : recent;
        int k = e->inputShift;   // digital silence holds the gain
        if (recent > 0) {
            k = 0;
            while (k < MBE_EXP_INPUT_MAXSHIFT && (recent << (k + 1)) <= MBE_EXP_INPUT_TARGET) ++k;
        }
        if (k != e->inputShift) {
            // the stored input to the new gain; |history| <= recent, so nothing saturates
            const int dk = k - e->inputShift;
            e->analysis.rescale_input(dk);
            for (int i = 0; i < e->frameAlign; ++i)
                e->delayLine[i] = static_cast<short>(dk > 0 ? e->delayLine[i] * (1 << dk) : e->delayLine[i] >> -dk);
            e->inputShift = k;
        }
        for (short& v : pcm) v = static_cast<short>(v * (1 << k));
    }
#endif
    const int shift = e->inputShift;
    e->analysisShift = shift;
    mbe::QuantizerOverride ovShifted;
    if (ov && ov->haveGamma && shift) { ovShifted = *ov; ovShifted.gamma += static_cast<float>(shift); ov = &ovShifted; }
    const float gainAdjust = e->opts.gain_adjust + static_cast<float>(shift);
    e->analysis.set_gain_adjust(static_cast<float>(shift));   // the IMBE quantiser (sa_encode)
#if MBE_FLOAT_ANALYSIS_ANY
    // the floating-point analysis needs no gain: it gets the input as it is (and keeps only the
    // shape of its amplitudes, the level coming from the fixed-point stage)
    if (usesFloatAnalysis(e->rate)) e->dstarAnalysis.push(raw);   // every frame, so its windows stay on the input's timeline
#endif
    short buf[160];
    const int A = e->frameAlign;
    std::memcpy(buf, e->delayLine, static_cast<size_t>(A) * sizeof(short));
    std::memcpy(buf + A, pcm, static_cast<size_t>(160 - A) * sizeof(short));
    std::memcpy(e->delayLine, pcm + (160 - A), static_cast<size_t>(A) * sizeof(short));
    short bufRaw[160];
    std::memcpy(bufRaw, e->delayLineRaw, static_cast<size_t>(A) * sizeof(short));
    std::memcpy(bufRaw + A, raw, static_cast<size_t>(160 - A) * sizeof(short));
    std::memcpy(e->delayLineRaw, raw + (160 - A), static_cast<size_t>(A) * sizeof(short));
    bool silent = true;
    for (short v : buf) if (v != 0) { silent = false; break; }

#if MBEVOCODER_IMBE_PITCH_ANALYSIS
    if (isImbe(e->rate)) imbeFloatAnalysis(e, buf, ov);
    else
#endif
    e->analysis.imbe_encode(e->frameVector, buf);
    e->pitchForced = false;   // an oracle pitch applies to one frame (the vocoder clears its own copy)
    e->frameFlags = MBE_ENCODER_FLAG_VOICE_ACTIVE;
    if (isImbe(e->rate)) {
        imbeVectorToData(e->frameVector, d);
        if (b) for (int i = 0; i < 8; ++i) b[i] = e->frameVector[i];
        return 0;
    }
    // Tone detection (docs/frontend.md): a tone frame replaces the voice frame; the analysis
    // and the quantiser's prediction state carry on as the decoder's do across a tone frame.
    // The detector sees the input delayed as the voice frames are (189 samples, 239 on D-STAR),
    // so a tone frame replaces the voice frame that would have described the same 20 ms.
    if (e->opts.tone_detect) {
        const int D = kAnalysisDelay + e->frameAlign;   // 189 / 239 > 160: the delayed frame is the oldest 160 stored samples
        static_assert(kAnalysisDelay + kFrameAlign2450 >= 160, "tone delay line shorter than a frame");
        short toneFrame[160];
        std::memcpy(toneFrame, e->toneDelay, 160 * sizeof(short));
        std::memmove(e->toneDelay, e->toneDelay + 160, static_cast<size_t>(D - 160) * sizeof(short));
        std::memcpy(e->toneDelay + (D - 160), toneRaw, 160 * sizeof(short));   // the AMBE-3000R's detector sees the input before suppression
        int toneId, amp;
        if (!silent && e->toneDetector.detect(toneFrame, &toneId, &amp)) {
            int ret = isAmbe2400(e->rate) ? mbe_encodeAmbe2400ToneData(toneId, amp, d) : mbe_encodeAmbe2450ToneData(toneId, amp, d);
            if (ret == 0) {   // D-STAR cannot carry KNOX tones: those stay voice, as on the AMBE-3000R
                e->frameFlags |= MBE_ENCODER_FLAG_TONE;
                if (b) { std::memset(b, 0, 9 * sizeof(int)); b[0] = toneId; b[1] = amp; }
                return 0;
            }
        }
    }
    int bb[9];
    // DTX (docs/frontend.md): background frames become silence frames, the flag says so.
    if (e->opts.dtx && !e->vad.process(bufRaw)) {
        e->frameFlags &= ~MBE_ENCODER_FLAG_VOICE_ACTIVE;
        if (!e->ambe.quantizeSilence(*e->analysis.param(), gainAdjust, e->opts.amplitude_floor, bb)) return MBE_STATUS_ENCODE_RANGE;
        tablesFor(e->rate).packFields(bb, d);
        if (b) std::memcpy(b, bb, sizeof bb);
        return 0;
    }
#if MBE_FLOAT_ANALYSIS_ANY
    mbe::DstarAnalysis* fa = usesFloatAnalysis(e->rate) ? &e->dstarAnalysis : nullptr;
#else
    mbe::DstarAnalysis* fa = nullptr;
#endif
    if (!e->ambe.quantize(*e->analysis.param(), silent, gainAdjust, e->opts.amplitude_floor, e->opts.voicing_threshold, bb, ov, fa)) return MBE_STATUS_ENCODE_RANGE;
    tablesFor(e->rate).packFields(bb, d);
    if (b) std::memcpy(b, bb, sizeof bb);
    return 0;
}

} // namespace

namespace mbe {

bool layoutMatchesRate(mbe_rate r, mbe_layout layout)
{
    switch (layout) {
    case MBE_LAYOUT_DSTAR: return isAmbe2400(r);
    case MBE_LAYOUT_DMR:
    case MBE_LAYOUT_P25P2: return isAmbe2450(r);
    case MBE_LAYOUT_P25P1:
    case MBE_LAYOUT_YSF_VW: return isImbe(r);
    default: return false;
    }
}

void encoderForcePitch(mbe_encoder* e, float periodSamples)
{
    if (!e) return;
    if (periodSamples < 21.0f || periodSamples > 122.5f) { e->analysis.set_forced_pitch(0); e->pitchForced = false; return; }
    e->analysis.set_forced_pitch(static_cast<int16_t>(periodSamples * 256.0f + 0.5f));
    e->pitchForced = true;
}

void encoderForceModel(mbe_encoder* e, const QuantizerOverride* ov)
{
    if (!e) return;
    e->haveOverride = ov != nullptr;
    if (!ov) return;
    e->override = *ov;
    // the quantiser and imbeApplyOverride index these arrays up to the counts
    QuantizerOverride& o = e->override;
    if (o.L < 0) o.L = 0;
    if (o.L > NUM_HARMS_MAX_OVERRIDE) o.L = NUM_HARMS_MAX_OVERRIDE;
    if (o.voicedL < 0) o.voicedL = 0;
    if (o.voicedL > NUM_HARMS_MAX_OVERRIDE) o.voicedL = NUM_HARMS_MAX_OVERRIDE;
}

void encoderFrontEndOutput(const mbe_encoder* e, short pcm[160])
{
    if (!e || !pcm) return;
    std::memcpy(pcm, e->frontEndOut, 160 * sizeof(short));
}

} // namespace mbe

extern "C" {

void mbe_encoderDefaultOptions(mbe_encoder_options* opts)
{
    if (!opts) return;
    std::memset(opts, 0, sizeof *opts);
    opts->gain_adjust = 0.0f;
    opts->amplitude_floor = 0.0f;
    opts->limiter = 0;
    opts->tone_detect = 1;
    opts->dtx = 0;
    opts->noise_suppress = 0;
    opts->echo_cancel = 0;
    for (float& v : opts->voicing_threshold) v = 0.0f;
}

int mbe_encoderFrameFlags(const mbe_encoder* e) { return e ? e->frameFlags : 0; }

int mbe_encoderSetEchoReference(mbe_encoder* e, const short far_end[160])
{
    if (!e) return MBE_STATUS_INVALID_ARGUMENT;
    if (far_end) { std::memcpy(e->farEnd, far_end, sizeof e->farEnd); e->haveFarEnd = true; }
    else e->haveFarEnd = false;
    return 0;
}

mbe_encoder* mbe_encoderCreate(mbe_rate rate, const mbe_encoder_options* opts)
{
    if (!supported(rate)) return nullptr;
    if (opts && opts->reserved != 0) return nullptr;   // documented as must-be-zero: reserved for later options
    mbe_encoder* e = new (std::nothrow) mbe_encoder(rate, tablesFor(rate));
    if (!e) return nullptr;
    if (opts) e->opts = *opts; else mbe_encoderDefaultOptions(&e->opts);
    std::memset(e->frameVector, 0, sizeof e->frameVector);
    e->frameAlign = isAmbe2400(rate) ? kFrameAlign2400 : kFrameAlign2450;
    if (isImbe(rate)) e->analysis.set_voicing_scale(kImbeVoicingScale);
#if MBEVOCODER_ENCODER_INPUT_GAIN
    e->analysis.set_wide_energy(true);   // the gain would saturate the 32-bit energies
    e->analysis.set_level_gate(kVoicingGateDb, kVoicingGateWidthDb);
#endif
    e->analysis.set_noise_gate(MBE_EXP_NOISE_GATE != 0);
#if MBE_FLOAT_ANALYSIS_ANY
    e->dstarAnalysis.setFrameAlign(e->frameAlign);
#endif
    return e;
}

void mbe_encoderDestroy(mbe_encoder* e) { delete e; }

void mbe_encoderReset(mbe_encoder* e)
{
    if (!e) return;
    e->analysis = imbe_vocoder();
    if (isImbe(e->rate)) e->analysis.set_voicing_scale(kImbeVoicingScale);
#if MBEVOCODER_ENCODER_INPUT_GAIN
    e->analysis.set_wide_energy(true);   // the gain would saturate the 32-bit energies
    e->analysis.set_level_gate(kVoicingGateDb, kVoicingGateWidthDb);
#endif
    e->analysis.set_noise_gate(MBE_EXP_NOISE_GATE != 0);
    e->ambe.reset();
    e->limiterEnv = 0.0f;
    std::memset(e->delayLine, 0, sizeof e->delayLine);
    std::memset(e->delayLineRaw, 0, sizeof e->delayLineRaw);
    std::memset(e->inputPeaks, 0, sizeof e->inputPeaks);
    e->inputShift = e->analysisShift = 0;
    e->toneDetector.reset();
    std::memset(e->toneDelay, 0, sizeof e->toneDelay);
    e->frameFlags = 0;
    e->vad.reset();
    e->echo.reset();
    e->haveFarEnd = false;
    e->noise.reset();
    std::memset(e->noiseRawDelay, 0, sizeof e->noiseRawDelay);
    std::memset(e->frontEndOut, 0, sizeof e->frontEndOut);
    std::memset(e->frameVector, 0, sizeof e->frameVector);
    e->haveOverride = false;
    e->pitchForced = false;
#if MBE_FLOAT_ANALYSIS_ANY
    e->dstarAnalysis.reset();
#endif
}

mbe_rate mbe_encoderRate(const mbe_encoder* e) { return e ? e->rate : MBE_RATE_COUNT; }

int mbe_encoderAnalysis(const mbe_encoder* e, mbe_parms* model)
{
    if (!e || !model) return MBE_STATUS_INVALID_ARGUMENT;
    const IMBE_PARAM* p = e->analysis.param();
    std::memset(model, 0, sizeof *model);
    // w0 from the reference pitch period (Q8.8 samples), not the vocoder's own fund_freq format.
    float period = static_cast<float>(p->ref_pitch) / 256.0f;
    model->w0 = period > 0 ? 2.0f * 3.14159265358979f / period : 0.0f;
    model->L = p->num_harms;
    const float unshift = std::ldexp(1.0f, -e->analysisShift);   // the input gain taken off
    for (int l = 1; l <= p->num_harms && l <= 56; ++l) {
        model->Vl[l] = p->v_uv_dsn[l - 1] ? 1 : 0;
        float sa = static_cast<float>(p->sa[l - 1]) * unshift;
        model->Ml[l] = sa;
        model->log2Ml[l] = sa > 0 ? std::log2(sa) : 0.0f;
    }
    return 0;
}

int mbe_encoderVoicingStats(const mbe_encoder* e, float ratio[57], float saV[57], float saU[57])
{
    if (!e) return MBE_STATUS_INVALID_ARGUMENT;
    const IMBE_PARAM* p = e->analysis.param();
    const int L = p->num_harms;
    if (ratio) ratio[0] = static_cast<float>(p->e_p) / 4096.0f;   // pitch estimation error (Q4.12)
    for (int l = 1; l <= L && l <= 56; ++l) {
        int band = (l - 1) / 3;
        if (band >= p->num_bands) band = p->num_bands - 1;
        if (band < 0) band = 0;
        if (ratio) {
            float thr = static_cast<float>(p->band_thr[band]);
            ratio[l] = thr > 0.0f ? static_cast<float>(p->band_dk[band]) / thr : 1e6f;
        }
        if (saV) saV[l] = std::ldexp(static_cast<float>(p->sa_v[l - 1]), -e->analysisShift);
        if (saU) saU[l] = std::ldexp(static_cast<float>(p->sa_u[l - 1]), -e->analysisShift);
    }
    return L;
}

int mbe_encoderDelaySamples(const mbe_encoder* e)
{
    if (!e) return MBE_STATUS_INVALID_ARGUMENT;
    return kAnalysisDelay + e->frameAlign + (e->opts.noise_suppress ? mbe::NoiseSuppressor::kFrame : 0);
}

int mbe_encoderParmBits(mbe_rate rate)
{
    if (isAmbe2450(rate) || rate == MBE_RATE_AMBE3600x2400) return 49;
    if (isImbe(rate)) return 88;
    return 0;
}

// 16-bit entry: apply the limiter (if on) to this call's samples, then analyse.
int encodeShort(mbe_encoder* e, const short pcm[160], char* d, int* b)
{
    if (!e->opts.limiter) return analyseAndQuantize(e, pcm, d, b);
    float x[160];
    for (int i = 0; i < 160; ++i) x[i] = static_cast<float>(pcm[i]);
    limitPeaks(e, x);
    short s[160];
    toShort(x, s);
    return analyseAndQuantize(e, s, d, b);
}

int mbe_encoderEncodeParms(mbe_encoder* e, const short pcm[160], char* d, int* b)
{
    if (!e || !pcm || !d) return MBE_STATUS_INVALID_ARGUMENT;
    return encodeShort(e, pcm, d, b);
}

int mbe_encoderEncodeParmsf(mbe_encoder* e, const float pcm[160], char* d, int* b)
{
    if (!e || !pcm || !d) return MBE_STATUS_INVALID_ARGUMENT;
    // Non-finite samples are silence: one infinite sample would otherwise hold the limiter's
    // envelope at infinity and mute the stream until a reset.
    float x[160];
    for (int i = 0; i < 160; ++i) x[i] = std::isfinite(pcm[i]) ? pcm[i] : 0.0f;
    if (e->opts.limiter) limitPeaks(e, x);
    short s[160];
    toShort(x, s);
    return analyseAndQuantize(e, s, d, b);
}

int mbe_encoderEncodeAmbeFrame(mbe_encoder* e, const short pcm[160], char fr[4][24])
{
    if (!e || !pcm || !fr || isImbe(e->rate)) return MBE_STATUS_INVALID_ARGUMENT;
    char d[49];
    int ret = encodeShort(e, pcm, d, nullptr);
    if (ret < 0) return ret;
    return isAmbe2400(e->rate) ? mbe_encodeAmbe3600x2400Frame(d, fr) : mbe_encodeAmbe3600x2450Frame(d, fr);
}

int mbe_encoderEncodeImbeFrame(mbe_encoder* e, const short pcm[160], char fr[8][23])
{
    if (!e || !pcm || !fr || !isImbe(e->rate)) return MBE_STATUS_INVALID_ARGUMENT;
    char d[88];
    int ret = encodeShort(e, pcm, d, nullptr);
    if (ret < 0) return ret;
    return mbe_encodeImbe7200x4400Frame(d, fr);
}

int mbe_encoderEncode(mbe_encoder* e, const short pcm[160], mbe_layout layout, char* bits)
{
    if (!e || !pcm || !bits) return MBE_STATUS_INVALID_ARGUMENT;
    if (!mbe::layoutMatchesRate(e->rate, layout)) return MBE_STATUS_INVALID_ARGUMENT;
    unsigned n = mbe_layoutBits(layout);
    if (n == 72) {
        char fr[4][24];
        int ret = mbe_encoderEncodeAmbeFrame(e, pcm, fr);
        if (ret < 0) return ret;
        return mbe_ambeFrameToBits(layout, fr, bits);
    }
    if (n == 144) {
        char fr[8][23];
        int ret = mbe_encoderEncodeImbeFrame(e, pcm, fr);
        if (ret < 0) return ret;
        return mbe_imbeFrameToBits(layout, fr, bits);
    }
    return MBE_STATUS_INVALID_ARGUMENT;
}

int mbe_encoderEncodeDvsi(mbe_encoder* e, const short pcm[160], uint8_t* bytes)
{
    if (!e || !pcm || !bytes) return MBE_STATUS_INVALID_ARGUMENT;
    if (e->rate == MBE_RATE_AMBE2450) {
        char d[49];
        int ret = encodeShort(e, pcm, d, nullptr);
        if (ret < 0) return ret;
        return mbe_ambe2450DataToDvsi(d, bytes);
    }
    if (e->rate == MBE_RATE_AMBE3600x2450) {
        char bits[72];
        int ret = mbe_encoderEncode(e, pcm, MBE_LAYOUT_DMR, bits);
        if (ret < 0) return ret;
        mbe_packBits(bits, 72, bytes, 1);
        return 0;
    }
    if (e->rate == MBE_RATE_AMBE3600x2400) {
        char bits[72];
        int ret = mbe_encoderEncode(e, pcm, MBE_LAYOUT_DSTAR, bits);
        if (ret < 0) return ret;
        mbe_packBits(bits, 72, bytes, 0);   // the AMBE-3000R packs D-STAR frames LSB first
        return 0;
    }
    return MBE_STATUS_INVALID_ARGUMENT;
}

} // extern "C"
