/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mbelib 1.x API (<mbelib/mbelib.h>), written as an mbelib 1.x client (DSD's calling
 * pattern), checked against the mbelib-neo API on the same frames: identical audio, errs /
 * errs2 equal to c0_errors / total_errors, err_str equal to mbe_formatProcessResult(), for
 * every rate, clean and damaged frames, frame and data paths, float and short output. The
 * first half of this file uses the mbelib 1.x names; the reference half, after the #undefs,
 * the mbelib-neo ones. */
#include <mbelib/mbelib.h>
#include <mbevocoder/mbevocoder.h>   /* frame assembly, to build the test frames */

#include <stdio.h>
#include <string.h>

#define FRAMES 40

static int failures = 0;
void refSynth(float* out, mbe_parms* cur, mbe_parms* prev);   /* reference half, below */
#define CHECK(c) do { if (!(c)) { ++failures; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* One stream's output through either API. */
typedef struct {
    float pcm[FRAMES][160];
    short pcm16[FRAMES][160];
    int errs[FRAMES], errs2[FRAMES];
    char err_str[FRAMES][MBE_V1_ERR_STR_SIZE];
} Run;

/* Frames: a voiced AMBE+2 / D-STAR / IMBE pattern, every fifth frame damaged in C0 and the
 * protected rows, frame 20 an erasure (AMBE+2) to exercise the flags. */
static void ambeFrames(int dstar, char fr[FRAMES][4][24], char d[FRAMES][49])
{
    for (int i = 0; i < FRAMES; ++i) {
        int b[9] = {30 + i % 20, 2 + i % 5, 10 + i % 15, 100 + 7 * i, 40 + i, 5 + i % 9, 3 + i % 11, 8, 2};
        if (dstar) { b[0] = 20 + i % 40; b[2] = 20 + i % 30; b[8] = 2 * (i % 4); }
        if (!dstar && i == 20) b[0] = 120;   /* erasure */
        CHECK((dstar ? mbe_packAmbe2400Fields : mbe_packAmbe2450Fields)(b, d[i]) == 0);
        CHECK((dstar ? mbe_encodeAmbe3600x2400Frame : mbe_encodeAmbe3600x2450Frame)(d[i], fr[i]) == 0);
        if (i % 5 == 4)
            for (int k = 0; k < 2 + i % 3; ++k) fr[i][k & 1][3 + 7 * k] ^= 1;
    }
}

static void imbeFrames(char fr[FRAMES][8][23], char d[FRAMES][88])
{
    for (int i = 0; i < FRAMES; ++i) {
        for (int k = 0; k < 88; ++k) d[i][k] = (char)(((k * 7 + i * 3) % 5) == 0);
        d[i][2] = (char)(i & 1);   /* vary b0 */
        CHECK(mbe_encodeImbe7200x4400Frame(d[i], fr[i]) == 0);
        if (i % 5 == 4)
            for (int k = 0; k < 2 + i % 4; ++k) fr[i][k % 4][2 + 5 * k] ^= 1;
    }
}

/* ---- the mbelib 1.x API, called as an mbelib 1.x client calls it ---- */

static void v1Ambe(int dstar, char fr[FRAMES][4][24], Run* out)
{
    mbe_setThreadRngSeed(1234u);   /* comfort noise is per thread: the same draw for both APIs */
    mbe_parms cur, prev, enh, cur16, prev16, enh16;
    char d[49];
    mbe_initMbeParms(&cur, &prev, &enh);
    mbe_initMbeParms(&cur16, &prev16, &enh16);
    for (int i = 0; i < FRAMES; ++i) {
        char f[4][24], f16[4][24];
        int e = 0, e2 = 0;
        char s[MBE_V1_ERR_STR_SIZE];
        memcpy(f, fr[i], sizeof f);
        memcpy(f16, fr[i], sizeof f16);
        if (dstar) {
            mbe_processAmbe3600x2400Framef(out->pcm[i], &out->errs[i], &out->errs2[i], out->err_str[i], f, d, &cur, &prev, &enh, 3);
            mbe_processAmbe3600x2400Frame(out->pcm16[i], &e, &e2, s, f16, d, &cur16, &prev16, &enh16, 3);
        } else {
            mbe_processAmbe3600x2450Framef(out->pcm[i], &out->errs[i], &out->errs2[i], out->err_str[i], f, d, &cur, &prev, &enh, 3);
            mbe_processAmbe3600x2450Frame(out->pcm16[i], &e, &e2, s, f16, d, &cur16, &prev16, &enh16, 3);
        }
        CHECK(e == out->errs[i] && e2 == out->errs2[i] && strcmp(s, out->err_str[i]) == 0);
    }
}

static void v1Imbe(char fr[FRAMES][8][23], Run* out)
{
    mbe_setThreadRngSeed(1234u);
    mbe_parms cur, prev, enh, cur16, prev16, enh16;
    char d[88];
    mbe_initMbeParms(&cur, &prev, &enh);
    mbe_initMbeParms(&cur16, &prev16, &enh16);
    for (int i = 0; i < FRAMES; ++i) {
        char f[8][23], f16[8][23];
        int e = 0, e2 = 0;
        char s[MBE_V1_ERR_STR_SIZE];
        memcpy(f, fr[i], sizeof f);
        memcpy(f16, fr[i], sizeof f16);
        mbe_processImbe7200x4400Framef(out->pcm[i], &out->errs[i], &out->errs2[i], out->err_str[i], f, d, &cur, &prev, &enh, 3);
        mbe_processImbe7200x4400Frame(out->pcm16[i], &e, &e2, s, f16, d, &cur16, &prev16, &enh16, 3);
        CHECK(e == out->errs[i] && e2 == out->errs2[i] && strcmp(s, out->err_str[i]) == 0);
    }
}

/* Data paths: the caller's own FEC counts go in through errs / errs2 and come back unchanged. */
static void v1Data(int rate, char d[FRAMES][88], const int* errs2In, Run* out)
{
    mbe_setThreadRngSeed(1234u);
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    for (int i = 0; i < FRAMES; ++i) {
        int e = 0, e2 = errs2In[i];
        if (rate == 0) mbe_processAmbe2450Dataf(out->pcm[i], &e, &e2, out->err_str[i], d[i], &cur, &prev, &enh, 3);
        else if (rate == 1) mbe_processAmbe2400Dataf(out->pcm[i], &e, &e2, out->err_str[i], d[i], &cur, &prev, &enh, 3);
        else mbe_processImbe4400Dataf(out->pcm[i], &e, &e2, out->err_str[i], d[i], &cur, &prev, &enh, 3);
        CHECK(e == 0 && e2 == errs2In[i]);
        out->errs[i] = e;
        out->errs2[i] = e2;
    }
}

static void v1Misc(void)
{
    char v[32];
    float pcm[160], pcm2[160];
    mbe_parms a, b, c, a2, b2, c2;
    mbe_printVersion(v);
    CHECK(strcmp(v, mbe_versionString()) == 0);
    /* mbe_synthesizeSpeechf with uvquality, against the same call without it below */
    mbe_initMbeParms(&a, &b, &c);
    a.w0 = 0.1f; a.L = 30;
    for (int l = 1; l <= 30; ++l) { a.Ml[l] = 1.0f / (float)l; a.Vl[l] = l < 20; }
    a2 = a; b2 = b; c2 = c;
    mbe_synthesizeSpeechf(pcm, &a, &b, 3);
    (void)c; (void)c2;
    refSynth(pcm2, &a2, &b2);
    CHECK(memcmp(pcm, pcm2, sizeof pcm) == 0);
}

/* ---- the reference: the mbelib-neo API ---- */
#undef mbe_processAmbe2400Dataf
#undef mbe_processAmbe3600x2400Framef
#undef mbe_processAmbe3600x2400Frame
#undef mbe_processAmbe2450Dataf
#undef mbe_processAmbe3600x2450Framef
#undef mbe_processAmbe3600x2450Frame
#undef mbe_processImbe4400Dataf
#undef mbe_processImbe7200x4400Framef
#undef mbe_processImbe7200x4400Frame
#undef mbe_synthesizeSpeechf

void refSynth(float* out, mbe_parms* cur, mbe_parms* prev) { mbe_synthesizeSpeechf(out, cur, prev); }

static void refAmbe(int dstar, char fr[FRAMES][4][24], Run* out)
{
    mbe_setThreadRngSeed(1234u);
    mbe_parms cur, prev, enh, cur16, prev16, enh16;
    char d[49];
    mbe_initMbeParms(&cur, &prev, &enh);
    mbe_initMbeParms(&cur16, &prev16, &enh16);
    for (int i = 0; i < FRAMES; ++i) {
        mbe_process_result r;
        mbe_initProcessResult(&r);
        if (dstar) {
            mbe_processAmbe3600x2400Framef(out->pcm[i], &r, (const char (*)[24])fr[i], d, &cur, &prev, &enh);
            mbe_processAmbe3600x2400Frame(out->pcm16[i], NULL, (const char (*)[24])fr[i], d, &cur16, &prev16, &enh16);
        } else {
            mbe_processAmbe3600x2450Framef(out->pcm[i], &r, (const char (*)[24])fr[i], d, &cur, &prev, &enh);
            mbe_processAmbe3600x2450Frame(out->pcm16[i], NULL, (const char (*)[24])fr[i], d, &cur16, &prev16, &enh16);
        }
        out->errs[i] = r.c0_errors;
        out->errs2[i] = r.total_errors;
        mbe_formatProcessResult(out->err_str[i], MBE_V1_ERR_STR_SIZE, &r);
    }
}

static void refImbe(char fr[FRAMES][8][23], Run* out)
{
    mbe_setThreadRngSeed(1234u);
    mbe_parms cur, prev, enh, cur16, prev16, enh16;
    char d[88];
    mbe_initMbeParms(&cur, &prev, &enh);
    mbe_initMbeParms(&cur16, &prev16, &enh16);
    for (int i = 0; i < FRAMES; ++i) {
        mbe_process_result r;
        mbe_initProcessResult(&r);
        mbe_processImbe7200x4400Framef(out->pcm[i], &r, (const char (*)[23])fr[i], d, &cur, &prev, &enh);
        mbe_processImbe7200x4400Frame(out->pcm16[i], NULL, (const char (*)[23])fr[i], d, &cur16, &prev16, &enh16);
        out->errs[i] = r.c0_errors;
        out->errs2[i] = r.total_errors;
        mbe_formatProcessResult(out->err_str[i], MBE_V1_ERR_STR_SIZE, &r);
    }
}

static void refData(int rate, char d[FRAMES][88], const int* errs2In, Run* out)
{
    mbe_setThreadRngSeed(1234u);
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    for (int i = 0; i < FRAMES; ++i) {
        mbe_process_result r;
        mbe_initProcessResult(&r);
        r.protected_errors = errs2In[i];
        r.total_errors = errs2In[i];
        if (rate == 0) mbe_processAmbe2450Dataf(out->pcm[i], &r, d[i], &cur, &prev, &enh);
        else if (rate == 1) mbe_processAmbe2400Dataf(out->pcm[i], &r, d[i], &cur, &prev, &enh);
        else mbe_processImbe4400Dataf(out->pcm[i], &r, d[i], &cur, &prev, &enh);
        mbe_formatProcessResult(out->err_str[i], MBE_V1_ERR_STR_SIZE, &r);
    }
}

static int same(const Run* a, const Run* b, int shorts, int counts, const char* what)
{
    int bad = 0;
    for (int i = 0; i < FRAMES; ++i) {
        if (memcmp(a->pcm[i], b->pcm[i], sizeof a->pcm[i]) != 0) ++bad;
        if (shorts && memcmp(a->pcm16[i], b->pcm16[i], sizeof a->pcm16[i]) != 0) ++bad;
        if (counts && (a->errs[i] != b->errs[i] || a->errs2[i] != b->errs2[i])) ++bad;
        if (strcmp(a->err_str[i], b->err_str[i]) != 0) ++bad;
    }
    if (bad) fprintf(stderr, "%s: %d mismatches\n", what, bad);
    return bad == 0;
}

static Run v1, ref;

int main(void)
{
    static char afr[FRAMES][4][24], ad[FRAMES][49], ifr[FRAMES][8][23], id[FRAMES][88], dd[FRAMES][88];
    int errs2In[FRAMES], anyErrors = 0, anyFlags = 0;
    for (int i = 0; i < FRAMES; ++i) errs2In[i] = (i % 7 == 6) ? 6 : i % 3;

    for (int dstar = 0; dstar < 2; ++dstar) {
        ambeFrames(dstar, afr, ad);
        memset(&v1, 0, sizeof v1); memset(&ref, 0, sizeof ref);
        v1Ambe(dstar, afr, &v1);
        refAmbe(dstar, afr, &ref);
        CHECK(same(&v1, &ref, 1, 1, dstar ? "D-STAR frames" : "AMBE+2 frames"));
        for (int i = 0; i < FRAMES; ++i) {
            anyErrors |= v1.errs2[i] > 0;
            anyFlags |= strchr(v1.err_str[i], 'E') || strchr(v1.err_str[i], 'R');
            CHECK((int)strspn(v1.err_str[i], "=") == v1.errs2[i]);
        }
        for (int i = 0; i < FRAMES; ++i) memcpy(dd[i], ad[i], 49);
        memset(&v1, 0, sizeof v1); memset(&ref, 0, sizeof ref);
        v1Data(dstar, dd, errs2In, &v1);
        refData(dstar, dd, errs2In, &ref);
        CHECK(same(&v1, &ref, 0, 0, dstar ? "D-STAR data" : "AMBE+2 data"));
    }
    imbeFrames(ifr, id);
    memset(&v1, 0, sizeof v1); memset(&ref, 0, sizeof ref);
    v1Imbe(ifr, &v1);
    refImbe(ifr, &ref);
    CHECK(same(&v1, &ref, 1, 1, "IMBE frames"));
    memset(&v1, 0, sizeof v1); memset(&ref, 0, sizeof ref);
    v1Data(2, id, errs2In, &v1);
    refData(2, id, errs2In, &ref);
    CHECK(same(&v1, &ref, 0, 0, "IMBE data"));
    CHECK(anyErrors && anyFlags);   /* the damaged frames did exercise the counts and flags */
    v1Misc();

    if (failures) { fprintf(stderr, "test_mbelib1_api: %d failure(s)\n", failures); return 1; }
    printf("test_mbelib1_api: ok\n");
    return 0;
}
