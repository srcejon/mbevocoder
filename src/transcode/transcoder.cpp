// SPDX-License-Identifier: GPL-3.0-or-later
// Parameter-domain transcoder: source parameter bits -> MBE model (through the library's
// own parameter decoder, so the model is exactly what a receiver would synthesise) ->
// requantised for the target rate with the target decoder in the closed loop.
#include <mbevocoder/mbevocoder.h>

#include "analysis/imbe_vocoder/imbe_vocoder.h"
#include "encoder/ambe_quant.hpp"
extern "C" {
#include "decoder/ambe_common.h"   // mbe_keepModel
}

#include <cmath>
#include <cstring>
#include <new>

namespace {

constexpr float kPi = 3.14159265358979f;

bool isAmbe2450(mbe_rate r) { return r == MBE_RATE_AMBE3600x2450 || r == MBE_RATE_AMBE2450; }
bool isAmbe2400(mbe_rate r) { return r == MBE_RATE_AMBE3600x2400; }
bool isImbe(mbe_rate r) { return r == MBE_RATE_IMBE7200x4400 || r == MBE_RATE_IMBE4400; }
bool valid(mbe_rate r) { return isAmbe2450(r) || isAmbe2400(r) || isImbe(r); }

int readBits(const char* d, int first, int count)
{
    int v = 0;
    for (int i = 0; i < count; ++i) v = (v << 1) | (d[first + i] & 1);
    return v;
}

// IMBE codewords u0..u7 (12,12,12,12,11,11,11,7 bits) -> imbe_d[88].
void imbeVectorToData(const int16_t u[8], char d[88])
{
    static const int widths[8] = {12, 12, 12, 12, 11, 11, 11, 7};
    int k = 0;
    for (int i = 0; i < 8; ++i)
        for (int bit = widths[i] - 1; bit >= 0; --bit) d[k++] = static_cast<char>((u[i] >> bit) & 1);
}

float safeLog2(float v) { return std::log2(v > 1e-6f ? v : 1e-6f); }

} // namespace

struct mbe_transcoder {
    mbe_rate from, to;
    mbe_parms cur{}, prev{}, enh{};   // source parameter decoder state
    mbe::AmbeQuantizer* ambe = nullptr;
    imbe_vocoder* imbe = nullptr;
    IMBE_PARAM ip{};
    int haveModel = 0;

    ~mbe_transcoder() { delete ambe; delete imbe; }
    bool reset()
    {
        mbe_initMbeParms(&cur, &prev, &enh);
        if (ambe) ambe->reset();
        if (imbe) *imbe = imbe_vocoder();   // in place: no allocation to fail
        haveModel = 0;
        return true;
    }
};

namespace {

// Decode the source frame into t->cur. Returns MBE_TRANSCODE_VOICE, MBE_TRANSCODE_TONE
// (tone id and amplitude filled) or MBE_TRANSCODE_REPEAT.
int decodeSource(mbe_transcoder* t, const char* d, int* toneId, int* toneAmp)
{
    *toneId = 0;
    *toneAmp = MBE_TONE_AMPLITUDE_DEFAULT;
    int ret;
    if (isAmbe2450(t->from)) {
        ret = mbe_decodeAmbe2450Parms(d, &t->cur, &t->prev);
        if (ret == 0) return MBE_TRANSCODE_VOICE;
        if (ret == 7) {   // tone frame: ID1 = u1[11:4], AD = u0[5:0]:u3[4]
            *toneId = readBits(d, 12, 8);
            *toneAmp = (readBits(d, 6, 6) << 1) | (d[44] & 1);
            return mbe_toneFrequencies(*toneId, nullptr, nullptr) ? MBE_TRANSCODE_TONE : MBE_TRANSCODE_REPEAT;
        }
        return MBE_TRANSCODE_REPEAT;
    }
    if (isAmbe2400(t->from)) {
        ret = mbe_decodeAmbe2400Parms(d, &t->cur, &t->prev);
        if (ret == 0) return MBE_TRANSCODE_VOICE;
        if (ret == 3 || (ret >= 5 && ret <= 122)) {
            // Tone (D-STAR numbering) or a D-STAR silence frame: re-read the index and volume.
            static const int t7[8] = {1, 0, 0, 0, 0, 1, 1, 1}, t6[8] = {0, 0, 0, 1, 1, 1, 1, 0}, t5[8] = {0, 0, 1, 0, 1, 1, 0, 1};
            int def = readBits(d, 6, 3);
            int id = (t7[def] << 7) | (t6[def] << 6) | (t5[def] << 5) | (d[9] << 4) | (d[42] << 3) | (d[43] << 2) | (d[10] << 1) | d[11];
            int vol = (readBits(d, 12, 5) << 3) | (d[44] << 2) | (d[45] << 1) | d[17];
            int canonical = mbe_toneIdFromDstar(id);
            if (canonical >= 0) {
                *toneId = canonical;
                int amp = (vol + 17) / 2;
                *toneAmp = amp > 127 ? 127 : amp;
                return MBE_TRANSCODE_TONE;
            }
            if (ret != 3) return MBE_TRANSCODE_REPEAT;
            // Silence: the decoder set a silence model (w0 for L = 14, all unvoiced) but no
            // amplitudes; use a quiet all-unvoiced frame.
            t->cur.gamma = 0;
            for (int l = 1; l <= t->cur.L; ++l) { t->cur.Vl[l] = 0; t->cur.Ml[l] = 1e-3f; t->cur.log2Ml[l] = safeLog2(1e-3f); }
            return MBE_TRANSCODE_VOICE;
        }
        return MBE_TRANSCODE_REPEAT;
    }
    ret = mbe_decodeImbe4400Parms(d, &t->cur, &t->prev);
    return ret == 0 ? MBE_TRANSCODE_VOICE : MBE_TRANSCODE_REPEAT;
}

// Decoder amplitude domain -> Yazev analysis amplitude scale (log2) for the IMBE quantiser,
// chosen so a frame transcoded to IMBE decodes at the level the source decoded at
// (docs/transcoder.md).
constexpr float kImbeLevel = 2.0f - 0.9f / 6.0206f;   // measured +0.9 dB at 2.0

int encodeImbeTarget(mbe_transcoder* t, const mbe_parms& m, char* dOut)
{
    // IMBE pitch: the decoder renders fundamental index b0 (0..207) as a period of (b0 + 39.5) / 2
    // samples, so code the nearest b0 and give the quantiser the ref_pitch (Q8.8 samples) that
    // codes it, the encoder's own grid. (Rounding the period to half samples coded round(2p) - 39,
    // which decodes a quarter sample long on average and moved an IMBE repeat up one
    // index.)
    const float period = 2.0f * kPi / m.w0;
    long b0 = std::lround(2.0f * period - 39.5f);
    if (b0 < 0) b0 = 0;
    if (b0 > 207) b0 = 207;
    const int16_t refPitch = static_cast<int16_t>((b0 + 39) * 128 + 64);
    IMBE_PARAM& p = t->ip;
    std::memset(&p, 0, sizeof p);
    p.ref_pitch = refPitch;
    const int Lt = imbe_vocoder::imbe_num_harms(refPitch);
    const float f0t = 256.0f / static_cast<float>(refPitch);
    const float f0s = m.w0 / (2.0f * kPi);
    const float ratio = f0t / f0s;
    // The decoders reconstruct Ml = sa / 4 for both voicing classes from the AMBE quantiser's
    // targets (the unvoiced analysis offset and the decoder's unvoiced scale cancel), so the
    // analysis-scale amplitude is 4 Ml, adjusted by the IMBE-specific level (kImbeLevel).
    for (int l = 1; l <= Lt; ++l) {
        float pos = static_cast<float>(l) * ratio;
        if (pos < 1.0f) pos = 1.0f;
        if (pos > static_cast<float>(m.L)) pos = static_cast<float>(m.L);
        int i0 = static_cast<int>(pos);
        int i1 = i0 < m.L ? i0 + 1 : i0;
        float frac = pos - static_cast<float>(i0);
        float la = (1.0f - frac) * safeLog2(m.Ml[i0]) + frac * safeLog2(m.Ml[i1]);
        int near = static_cast<int>(pos + 0.5f);
        if (near < 1) near = 1;
        if (near > m.L) near = m.L;
        int v = m.Vl[near] ? 1 : 0;
        float logSa = la + kImbeLevel;
        float sa = std::exp2(logSa);
        if (sa < 1.0f) sa = 1.0f;
        if (sa > 32767.0f) sa = 32767.0f;
        p.sa[l - 1] = static_cast<int16_t>(sa + 0.5f);
        p.v_uv_dsn[l - 1] = static_cast<int16_t>(v);
    }
    int16_t u[8];
    t->imbe->imbe_encode_model(&p, u);
    imbeVectorToData(u, dOut);
    return 0;
}

// Quantise the model m for the target rate into dOut; returns kind or a status.
int transcodeModel(mbe_transcoder* t, const mbe_parms& m, int kind, char* dOut)
{
    if (isImbe(t->to)) return encodeImbeTarget(t, m, dOut) < 0 ? MBE_STATUS_ENCODE_RANGE : kind;

    // Level: the AMBE-3000R's two AMBE decoders render the same model within half a dB of
    // each other (docs/transcoder.md: +0.3 dB one way, -0.45 dB the other, 24 files; the
    // 3.3 dB seen before the D-STAR gain dynamics were corrected was an artefact of them), so
    // shift the gain by that so a bridged call keeps its loudness on real radios. Positive
    // values lower the target gain (log2 units).
    float gainAdjust = 0.0f;
    if (isAmbe2400(t->to) && !isAmbe2400(t->from)) gainAdjust = 0.3f / 6.0206f;
    if (isAmbe2450(t->to) && isAmbe2400(t->from)) gainAdjust = -0.45f / 6.0206f;
    int b[9];
    // The source's mixed-excitation fractions, when its decoder measured them, pick the
    // target voicing row by what the decoders render (docs/transcoder.md).
    if (!t->ambe->quantizeModel(m.w0, m.L, m.Vl, m.Ml, gainAdjust, b, m.vmixValid ? m.Vmix : nullptr)) return MBE_STATUS_ENCODE_RANGE;
    t->ambe->tables().packFields(b, dOut);
    return kind;
}

} // namespace

extern "C" {

mbe_transcoder* mbe_transcoderCreate(mbe_rate from, mbe_rate to)
{
    if (!valid(from) || !valid(to)) return nullptr;
    mbe_transcoder* t = new (std::nothrow) mbe_transcoder;
    if (!t) return nullptr;
    t->from = from;
    t->to = to;
    if (isImbe(to)) t->imbe = new (std::nothrow) imbe_vocoder();
    else t->ambe = new (std::nothrow) mbe::AmbeQuantizer(isAmbe2400(to) ? mbe::ambe2400Tables() : mbe::ambe2450Tables());
    if ((isImbe(to) && !t->imbe) || (!isImbe(to) && !t->ambe) || !t->reset()) { delete t; return nullptr; }
    return t;
}

void mbe_transcoderDestroy(mbe_transcoder* t) { delete t; }
void mbe_transcoderReset(mbe_transcoder* t) { if (t) t->reset(); }
mbe_rate mbe_transcoderSourceRate(const mbe_transcoder* t) { return t ? t->from : MBE_RATE_COUNT; }
mbe_rate mbe_transcoderTargetRate(const mbe_transcoder* t) { return t ? t->to : MBE_RATE_COUNT; }
const mbe_parms* mbe_transcoderModel(const mbe_transcoder* t) { return t && t->haveModel ? &t->prev : nullptr; }

int mbe_transcoderProcess(mbe_transcoder* t, const char* dIn, char* dOut)
{
    if (!t || !dIn || !dOut) return MBE_STATUS_INVALID_ARGUMENT;
    const int inBits = mbe_encoderParmBits(t->from), outBits = mbe_encoderParmBits(t->to);
    for (int i = 0; i < inBits; ++i) if ((dIn[i] & ~1) != 0) return MBE_STATUS_INVALID_BITS;

    const bool sameFormat = (isAmbe2450(t->from) && isAmbe2450(t->to)) || (isAmbe2400(t->from) && isAmbe2400(t->to))
                            || (isImbe(t->from) && isImbe(t->to));
    int toneId, toneAmp;
    int kind = decodeSource(t, dIn, &toneId, &toneAmp);
    if (sameFormat && kind != MBE_TRANSCODE_REPEAT) {
        // Same parameter format on both sides: voice and tone frames pass through unchanged
        // (the model state is kept in step for voice). An erasure takes the repeat path below
        // like any other rate pair, so the output is always a usable frame.
        std::memcpy(dOut, dIn, static_cast<size_t>(outBits));
        if (kind == MBE_TRANSCODE_VOICE && !t->cur.silenceFrame) {   // a silence frame is not the state (below)
            mbe_moveMbeParms(&t->cur, &t->prev);
            t->haveModel = 1;
        }
        return kind;
    }
    if (kind == MBE_TRANSCODE_TONE) {
        if (isImbe(t->to)) {
            kind = MBE_TRANSCODE_REPEAT;   // IMBE has no tone frames
        } else {
            int ret = isAmbe2400(t->to) ? mbe_encodeAmbe2400ToneData(toneId, toneAmp, dOut)
                                        : mbe_encodeAmbe2450ToneData(toneId, toneAmp, dOut);
            if (ret == 0) return MBE_TRANSCODE_TONE;
            if (ret != MBE_STATUS_ENCODE_RANGE) return ret;
            kind = MBE_TRANSCODE_REPEAT;   // the target rate cannot carry this tone (KNOX on D-STAR)
        }
    }
    if (kind == MBE_TRANSCODE_REPEAT) {
        if (!t->haveModel) {
            // Nothing to repeat yet: a quiet all-unvoiced frame.
            mbe_initMbeParms(&t->cur, &t->prev, &t->enh);
            t->cur.w0 = 2.0f * kPi / 32.0f;
            t->cur.L = 14;
            for (int l = 1; l <= 14; ++l) { t->cur.Vl[l] = 0; t->cur.Ml[l] = 1e-3f; t->cur.log2Ml[l] = safeLog2(1e-3f); }
        } else {
            mbe_useLastMbeParms(&t->cur, &t->prev);
        }
    }
    // Advance the source decoder's prediction state as its process function would: an
    // AMBE+2 silence frame (DTX) is rendered from the last voice frame and does not become
    // the state (the decoder's freeze, docs/frontend.md), so it is put back afterwards.
    mbe_parms keep;
    const int freeze = t->cur.silenceFrame;
    if (freeze) mbe_moveMbeParms(&t->prev, &keep);
    mbe_moveMbeParms(&t->cur, &t->prev);
    t->haveModel = 1;
    const mbe_parms& m = t->prev;
    const int ret = transcodeModel(t, m, kind, dOut);
    if (freeze) mbe_keepModel(&keep, &t->prev);
    return ret;
}

} // extern "C"
