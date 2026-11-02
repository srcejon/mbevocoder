/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mbelib-neo 2.4's speech encoders for tools/quality/neo_encoders.py: reads a 16-bit mono 8 kHz WAV,
   encodes it with mbe_encodeAmbe2400ParmsShort / mbe_encodeAmbe2450ParmsShort /
   mbe_encodeImbe4400ParmsShort and mbelib-neo's own frame assembly, and writes one frame per line
   as mbe-cli frbits does (ambe_fr[4][24] or imbe_fr[8][23] as 0/1, row-major); `mbe-cli frpack`
   turns that into channel bytes. Four frames of zeros follow the input to flush the tail. */
#include <mbelib-neo/mbelib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static short* readWav(const char* path, long* count)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return NULL; }
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) { fclose(f); return NULL; }
        unsigned long n = c[4] | (c[5] << 8) | ((unsigned long)c[6] << 16) | ((unsigned long)c[7] << 24);
        if (!memcmp(c, "data", 4)) {
            short* s = (short*)malloc(n + 2 * 160);
            *count = (long)(fread(s, 1, n, f) / 2);
            fclose(f);
            return s;
        }
        fseek(f, (long)(n + (n & 1)), SEEK_CUR);
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: neoenc dstar|ambe2450|imbe7200 in.wav out.fr.txt\n"); return 2; }
    int dstar = strcmp(argv[1], "dstar") == 0, imbe = strcmp(argv[1], "imbe7200") == 0;
    long n = 0;
    short* pcm = readWav(argv[2], &n);
    FILE* out = fopen(argv[3], "w");
    if (!pcm || !out) { fprintf(stderr, "cannot open input or output\n"); return 1; }
    mbe_ambe2400_encoder* e24 = dstar ? mbe_ambe2400EncoderAlloc() : NULL;
    mbe_ambe2450_encoder* e45 = (!dstar && !imbe) ? mbe_ambe2450EncoderAlloc() : NULL;
    mbe_imbe4400_encoder* ei = imbe ? mbe_imbe4400EncoderAlloc() : NULL;
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    long frames = (n + 159) / 160 + 4;
    for (long k = 0; k < frames; ++k) {
        short s[160];
        for (int i = 0; i < 160; ++i) s[i] = (k * 160 + i < n) ? pcm[k * 160 + i] : 0;
        int r;
        if (imbe) {
            char d[88], fr[8][23];
            r = mbe_encodeImbe4400ParmsShort(ei, s, d, &cur, &prev);
            if (r >= 0) r = mbe_encodeImbe7200x4400Frame(d, fr);
            if (r < 0) { fprintf(stderr, "frame %ld: status %d\n", k, r); return 1; }
            for (int i = 0; i < 184; ++i) fputc(fr[i / 23][i % 23] ? '1' : '0', out);
        } else {
            char d[49], fr[4][24];
            r = dstar ? mbe_encodeAmbe2400ParmsShort(e24, s, d, &cur, &prev) : mbe_encodeAmbe2450ParmsShort(e45, s, d, &cur, &prev);
            if (r >= 0) r = dstar ? mbe_encodeAmbe3600x2400Frame(d, fr) : mbe_encodeAmbe3600x2450Frame(d, fr);
            if (r < 0) { fprintf(stderr, "frame %ld: status %d\n", k, r); return 1; }
            for (int i = 0; i < 96; ++i) fputc(fr[i / 24][i % 24] ? '1' : '0', out);
        }
        fputc('\n', out);
        mbe_moveMbeParms(&cur, &prev);
    }
    fclose(out);
    if (e24) mbe_ambe2400EncoderFree(e24);
    if (e45) mbe_ambe2450EncoderFree(e45);
    if (ei) mbe_imbe4400EncoderFree(ei);
    free(pcm);
    return 0;
}
