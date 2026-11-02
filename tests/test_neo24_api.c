/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mbelib-neo 2.4's additions to <mbelib-neo/mbelib.h>, from C with that header alone: the D-STAR DV
 * data helpers, mbe_classifyAmbe2450Frame, the SILENCE and PROVOICE result flags, and the encoder
 * API (its contract: cur_mp is the decoder's model of the emitted bits given prev_mp, prev_mp is
 * only read, a tone frame leaves the history alone, invalid input is rejected). */
#include <mbelib-neo/mbelib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void setBits(char* d, int first, int count, unsigned v)   /* MSB first */
{
    for (int i = 0; i < count; ++i) d[first + i] = (char)((v >> (count - 1 - i)) & 1u);
}

static void setB0(char d[49], int b0)   /* AMBE+2 b0: d[0..3] high bits, d[37..39] low bits */
{
    setBits(d, 0, 4, (unsigned)b0 >> 3);
    setBits(d, 37, 3, (unsigned)b0 & 7u);
}

static void dvData(void)
{
    char fr[4][24], back[4][24];
    unsigned char bytes[9];
    memset(fr, 0, sizeof fr);
    for (int i = 0; i < 96; ++i) {
        int r = i / 24, c = i % 24;
        int used = (r == 0) || (r == 1 && c < 23) || (r == 2 && c < 11) || (r == 3 && c < 14);
        fr[r][c] = (char)(used && ((i * 7 + 3) % 5 < 2));
    }
    CHECK(mbe_encodeDStarDVData((const char (*)[24])fr, bytes) == 0);
    CHECK(mbe_decodeDStarDVData(bytes, back) == 0);
    CHECK(memcmp(fr, back, sizeof fr) == 0);
    CHECK(mbe_encodeDStarDVData(NULL, bytes) == MBE_STATUS_INVALID_ARGUMENT);
    CHECK(mbe_decodeDStarDVData(NULL, back) == MBE_STATUS_INVALID_ARGUMENT);
}

static void classify(void)
{
    char d[49];
    memset(d, 0, sizeof d);
    setB0(d, 10);
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_VOICE);
    setB0(d, 124);
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_SILENCE);
    setB0(d, 125);
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_SILENCE);
    setB0(d, 121);
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_ERASURE);
    /* tone frames: u0's first six bits set, the tone id in d[12..19], u3's last nibble 0 */
    memset(d, 0, sizeof d);
    setBits(d, 0, 6, 63u);
    setBits(d, 12, 8, 128u);   /* DTMF 0 */
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_TONE);
    setBits(d, 12, 8, 255u);   /* the silent tone */
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_TONE);
    setBits(d, 12, 8, 200u);   /* no such tone */
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_ERASURE);
    setBits(d, 12, 8, 128u);
    setBits(d, 45, 4, 5u);     /* u3's nibble set and u1's two copies of the id's high nibble differ */
    setBits(d, 20, 4, 3u);
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_ERASURE);
    d[0] = 2;
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_STATUS_INVALID_BITS);
}

static void flags(void)
{
    mbe_parms cur, prev, enh;
    mbe_process_result r;
    float pcm[160];
    char d[49];
    /* a voice frame, then a silence frame: played, flagged, not the prediction state */
    mbe_initMbeParms(&cur, &prev, &enh);
    memset(d, 0, sizeof d);
    setB0(d, 40);
    mbe_initProcessResult(&r);
    CHECK(mbe_processAmbe2450Dataf(pcm, &r, d, &cur, &prev, &enh) >= 0);
    CHECK((r.flags & MBE_PROCESS_FLAG_SILENCE) == 0u);
    setB0(d, 124);
    mbe_initProcessResult(&r);
    CHECK(mbe_processAmbe2450Dataf(pcm, &r, d, &cur, &prev, &enh) >= 0);
    CHECK((r.flags & MBE_PROCESS_FLAG_SILENCE) != 0u);
    CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_SILENCE);

    /* a ProVoice frame decode carries the PROVOICE context into the IMBE data path */
    char fr[7][24], id[88];
    memset(fr, 0, sizeof fr);
    mbe_initMbeParms(&cur, &prev, &enh);
    CHECK(mbe_decodeImbe7100x4400Frame((const char (*)[24])fr, id, &r) >= 0);
    CHECK((r.flags & MBE_PROCESS_FLAG_PROVOICE) != 0u);
    CHECK(mbe_processImbe4400Dataf(pcm, &r, id, &cur, &prev, &enh) >= 0);
    CHECK((r.flags & MBE_PROCESS_FLAG_PROVOICE) != 0u);
    CHECK(mbe_processImbe7100x4400Framef(pcm, &r, (const char (*)[24])fr, id, &cur, &prev, &enh) >= 0);
    CHECK((r.flags & MBE_PROCESS_FLAG_PROVOICE) != 0u);
}

/* A voiced test signal on mbelib-neo's float scale, or a DTMF digit. */
static void signal(float* x, long n0, int dtmf)
{
    for (int i = 0; i < 160; ++i) {
        double t = (double)(n0 + i) / 8000.0, v = 0;
        if (dtmf) v = 0.2 * (sin(2 * M_PI * 697 * t) + sin(2 * M_PI * 1209 * t));
        else for (int h = 1; h <= 20; ++h) v += 0.3 / h * sin(2 * M_PI * 140 * h * t + h);
        x[i] = (float)v;
    }
}

typedef int (*decodeFn)(const char*, mbe_parms*, mbe_parms*);

/* cur_mp against the decoder's own model of the bits, decoded from a copy of prev_mp */
static int sameModel(decodeFn dec, const char* d, const mbe_parms* cur, const mbe_parms* prev)
{
    mbe_parms c, p;
    memcpy(&c, prev, sizeof c);
    memcpy(&p, prev, sizeof p);
    if (dec(d, &c, &p) < 0) return 0;
    if (c.w0 != cur->w0 || c.L != cur->L || c.gamma != cur->gamma) return 0;
    for (int l = 1; l <= c.L; ++l)
        if (c.Vl[l] != cur->Vl[l] || c.log2Ml[l] != cur->log2Ml[l]) return 0;
    return 1;
}

static void encoders(void)
{
    mbe_ambe2400_encoder* e24 = mbe_ambe2400EncoderAlloc();
    mbe_ambe2450_encoder* e45 = mbe_ambe2450EncoderAlloc();
    mbe_imbe4400_encoder* ei = mbe_imbe4400EncoderAlloc();
    CHECK(e24 && e45 && ei);
    if (!e24 || !e45 || !ei) return;
    for (int which = 0; which < 3; ++which) {
        mbe_parms cur, prev, enh, before;
        char d[88];
        float x[160];
        short s[160];
        int voiced = 0;
        decodeFn dec = which == 0 ? mbe_decodeAmbe2400Parms : which == 1 ? mbe_decodeAmbe2450Parms : mbe_decodeImbe4400Parms;
        mbe_initMbeParms(&cur, &prev, &enh);
        for (long k = 0; k < 60; ++k) {
            signal(x, k * 160, 0);
            for (int i = 0; i < 160; ++i) s[i] = (short)lrintf(x[i] * 32767.0f);
            memcpy(&before, &prev, sizeof before);
            int r = which == 0 ? ((k & 1) ? mbe_encodeAmbe2400Parms(e24, x, d, &cur, &prev) : mbe_encodeAmbe2400ParmsShort(e24, s, d, &cur, &prev))
                  : which == 1 ? ((k & 1) ? mbe_encodeAmbe2450Parms(e45, x, d, &cur, &prev) : mbe_encodeAmbe2450ParmsShort(e45, s, d, &cur, &prev))
                               : ((k & 1) ? mbe_encodeImbe4400Parms(ei, x, d, &cur, &prev) : mbe_encodeImbe4400ParmsShort(ei, s, d, &cur, &prev));
            CHECK(r == 0);   /* voice (MBE_AMBE2450_FRAME_VOICE is 0) */
            CHECK(memcmp(&before, &prev, sizeof before) == 0);   /* prev_mp is only read */
            CHECK(sameModel(dec, d, &cur, &prev));
            for (int l = 1; l <= cur.L; ++l) voiced += cur.Vl[l];
            mbe_moveMbeParms(&cur, &prev);
        }
        CHECK(voiced > 0);
    }
    /* a DTMF digit becomes AMBE+2 tone frames, which leave the history alone */
    {
        mbe_parms cur, prev, enh;
        char d[49];
        float x[160];
        int tones = 0;
        mbe_ambe2450EncoderReset(e45);
        mbe_initMbeParms(&cur, &prev, &enh);
        for (long k = 0; k < 30; ++k) {
            signal(x, k * 160, 1);
            int r = mbe_encodeAmbe2450Parms(e45, x, d, &cur, &prev);
            CHECK(r == MBE_AMBE2450_FRAME_VOICE || r == MBE_AMBE2450_FRAME_TONE);
            if (r == MBE_AMBE2450_FRAME_TONE) {
                ++tones;
                CHECK(mbe_classifyAmbe2450Frame(d) == MBE_AMBE2450_FRAME_TONE);
                CHECK(memcmp(&cur, &prev, sizeof cur) == 0);
            }
            mbe_moveMbeParms(&cur, &prev);
        }
        CHECK(tones > 15);
    }
    /* invalid input */
    {
        mbe_parms cur, prev, enh;
        char d[88];   /* the IMBE call below writes 88 */
        float x[160];
        mbe_initMbeParms(&cur, &prev, &enh);
        signal(x, 0, 0);
        CHECK(mbe_encodeAmbe2450Parms(NULL, x, d, &cur, &prev) == MBE_STATUS_INVALID_ARGUMENT);
        CHECK(mbe_encodeAmbe2450Parms(e45, NULL, d, &cur, &prev) == MBE_STATUS_INVALID_ARGUMENT);
        CHECK(mbe_encodeAmbe2450Parms(e45, x, d, &prev, &prev) == MBE_STATUS_INVALID_ARGUMENT);
        x[17] = NAN;
        CHECK(mbe_encodeAmbe2450Parms(e45, x, d, &cur, &prev) == MBE_STATUS_INVALID_ARGUMENT);
        x[17] = 2e6f;
        CHECK(mbe_encodeAmbe2400Parms(e24, x, d, &cur, &prev) == MBE_STATUS_INVALID_ARGUMENT);
        x[17] = 0;
        prev.gamma = INFINITY;
        CHECK(mbe_encodeImbe4400Parms(ei, x, d, &cur, &prev) == MBE_STATUS_INVALID_ARGUMENT);
    }
    mbe_ambe2400EncoderFree(e24);
    mbe_ambe2450EncoderFree(e45);
    mbe_imbe4400EncoderFree(ei);
    mbe_ambe2450EncoderFree(NULL);
    mbe_ambe2400EncoderReset(NULL);
}

int main(void)
{
    dvData();
    classify();
    flags();
    encoders();
    mbe_freeThreadResources();
    if (failures) fprintf(stderr, "test_neo24_api: %d failure(s)\n", failures);
    else printf("test_neo24_api: ok\n");
    return failures != 0;
}
