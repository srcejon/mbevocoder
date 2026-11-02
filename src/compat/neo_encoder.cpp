// SPDX-License-Identifier: GPL-3.0-or-later
// mbelib-neo 2.4's encoder API (<mbelib-neo/mbelib.h>) on this library's encoders: one
// mbe_encoder per context, the parameter bits it produces, and cur_mp as the decoder decodes them
// from prev_mp, which is what mbelib-neo's contract promises its callers.
#include <mbevocoder/mbevocoder.h>

#include <cmath>
#include <cstring>
#include <new>

namespace {

struct NeoEncoder {
    mbe_encoder* e = nullptr;
    mbe_rate rate = MBE_RATE_COUNT;
    char last[88] = {0};   // the last frame produced, repeated if a frame cannot be coded
    bool haveLast = false;
};

NeoEncoder* create(mbe_rate rate)
{
    NeoEncoder* n = new (std::nothrow) NeoEncoder;
    if (!n) return nullptr;
    n->e = mbe_encoderCreate(rate, nullptr);   // the defaults: tone frames on, no DTX, as mbelib-neo
    if (!n->e) { delete n; return nullptr; }
    n->rate = rate;
    return n;
}

void reset(NeoEncoder* n)
{
    if (!n) return;
    mbe_encoderReset(n->e);
    n->haveLast = false;
}

void destroy(NeoEncoder* n)
{
    if (!n) return;
    mbe_encoderDestroy(n->e);
    delete n;
}

constexpr float kLimit = 1048576.0f;   // 2^20: mbelib-neo's bound on samples and on the state's levels

// mbelib-neo rejects a previous state it could not predict from without overflow.
bool stateUsable(const mbe_parms* p)
{
    if (!std::isfinite(p->gamma) || std::fabs(p->gamma) > kLimit) return false;
    for (float v : p->log2Ml)
        if (!std::isfinite(v) || std::fabs(v) > kLimit || v > 120.0f) return false;
    return true;
}

// cur_mp: the decoder's model of the frame given prev_mp (a copy: the decoders write edge values
// into prev_mp), or prev_mp itself after a tone frame, which leaves the prediction history alone.
int finish(NeoEncoder* n, int ret, char* d, mbe_parms* cur, const mbe_parms* prev)
{
    const int nbits = mbe_encoderParmBits(n->rate);
    if (ret == MBE_STATUS_ENCODE_RANGE && n->haveLast) {
        std::memcpy(d, n->last, static_cast<size_t>(nbits));   // mbelib-neo always produces a frame
        ret = 0;
    }
    if (ret < 0) return ret;
    std::memcpy(n->last, d, static_cast<size_t>(nbits));
    n->haveLast = true;
    if (mbe_encoderFrameFlags(n->e) & MBE_ENCODER_FLAG_TONE) {
        mbe_moveMbeParms(prev, cur);
        return n->rate == MBE_RATE_AMBE2450 ? MBE_AMBE2450_FRAME_TONE : 0;
    }
    mbe_parms p = *prev;
    *cur = *prev;
    int dec = n->rate == MBE_RATE_AMBE3600x2400 ? mbe_decodeAmbe2400Parms(d, cur, &p)
            : n->rate == MBE_RATE_AMBE2450      ? mbe_decodeAmbe2450Parms(d, cur, &p)
                                                : mbe_decodeImbe4400Parms(d, cur, &p);
    if (dec < 0) return dec;
    return n->rate == MBE_RATE_AMBE2450 ? MBE_AMBE2450_FRAME_VOICE : 0;
}

int encodeShort(NeoEncoder* n, const short* pcm, char* d, mbe_parms* cur, const mbe_parms* prev)
{
    if (!n || !pcm || !d || !cur || !prev || cur == prev || !stateUsable(prev)) return MBE_STATUS_INVALID_ARGUMENT;
    return finish(n, mbe_encoderEncodeParms(n->e, pcm, d, nullptr), d, cur, prev);
}

// Float input on mbelib-neo's scale (nominal +-1); this library's float path takes the 16-bit scale
// and clips to it.
int encodeFloat(NeoEncoder* n, const float* pcm, char* d, mbe_parms* cur, const mbe_parms* prev)
{
    if (!n || !pcm || !d || !cur || !prev || cur == prev || !stateUsable(prev)) return MBE_STATUS_INVALID_ARGUMENT;
    float x[160];
    for (int i = 0; i < 160; ++i) {
        if (!std::isfinite(pcm[i]) || std::fabs(pcm[i]) > kLimit) return MBE_STATUS_INVALID_ARGUMENT;
        x[i] = pcm[i] * 32768.0f;
    }
    return finish(n, mbe_encoderEncodeParmsf(n->e, x, d, nullptr), d, cur, prev);
}

} // namespace

struct mbe_ambe2400_encoder : NeoEncoder {};
struct mbe_ambe2450_encoder : NeoEncoder {};
struct mbe_imbe4400_encoder : NeoEncoder {};

extern "C" {

mbe_ambe2400_encoder* mbe_ambe2400EncoderAlloc(void) { return static_cast<mbe_ambe2400_encoder*>(create(MBE_RATE_AMBE3600x2400)); }
mbe_ambe2450_encoder* mbe_ambe2450EncoderAlloc(void) { return static_cast<mbe_ambe2450_encoder*>(create(MBE_RATE_AMBE2450)); }
mbe_imbe4400_encoder* mbe_imbe4400EncoderAlloc(void) { return static_cast<mbe_imbe4400_encoder*>(create(MBE_RATE_IMBE4400)); }
void mbe_ambe2400EncoderReset(mbe_ambe2400_encoder* enc) { reset(enc); }
void mbe_ambe2450EncoderReset(mbe_ambe2450_encoder* enc) { reset(enc); }
void mbe_imbe4400EncoderReset(mbe_imbe4400_encoder* enc) { reset(enc); }
void mbe_ambe2400EncoderFree(mbe_ambe2400_encoder* enc) { destroy(enc); }
void mbe_ambe2450EncoderFree(mbe_ambe2450_encoder* enc) { destroy(enc); }
void mbe_imbe4400EncoderFree(mbe_imbe4400_encoder* enc) { destroy(enc); }

int mbe_encodeAmbe2400Parms(mbe_ambe2400_encoder* enc, const float* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp)
{ return encodeFloat(enc, samples, ambe_d, cur_mp, prev_mp); }
int mbe_encodeAmbe2400ParmsShort(mbe_ambe2400_encoder* enc, const short* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp)
{ return encodeShort(enc, samples, ambe_d, cur_mp, prev_mp); }
int mbe_encodeAmbe2450Parms(mbe_ambe2450_encoder* enc, const float* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp)
{ return encodeFloat(enc, samples, ambe_d, cur_mp, prev_mp); }
int mbe_encodeAmbe2450ParmsShort(mbe_ambe2450_encoder* enc, const short* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp)
{ return encodeShort(enc, samples, ambe_d, cur_mp, prev_mp); }
int mbe_encodeImbe4400Parms(mbe_imbe4400_encoder* enc, const float* samples, char imbe_d[88], mbe_parms* cur_mp, const mbe_parms* prev_mp)
{ return encodeFloat(enc, samples, imbe_d, cur_mp, prev_mp); }
int mbe_encodeImbe4400ParmsShort(mbe_imbe4400_encoder* enc, const short* samples, char imbe_d[88], mbe_parms* cur_mp, const mbe_parms* prev_mp)
{ return encodeShort(enc, samples, imbe_d, cur_mp, prev_mp); }

} // extern "C"
