/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mbelib-neo 2.x decoder driver for the lineage and DVSI comparisons: reads ambe_fr[4][24] /
   imbe_fr[8][23] bit lines (mbe-cli frbits), or with "soft" dvsisoft's soft-frame lines, and writes
   raw float32, as tools/mbelib1-decode does for mbelib 1.x. */
#include <mbelib-neo/mbelib.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: neodec dstar|ambe2450|imbe7200 fr.txt out.f32\n"); return 2; }
    int dstar = strcmp(argv[1], "dstar") == 0, imbe = strcmp(argv[1], "imbe7200") == 0;
    int soft = argc > 4 && strcmp(argv[4], "soft") == 0;
    FILE* in = fopen(argv[2], "r"); FILE* out = fopen(argv[3], "wb");
    if (!in || !out) return 1;
    mbe_parms cur, prev, enh; mbe_initMbeParms(&cur, &prev, &enh);
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        float pcm[160]; mbe_process_result r; mbe_initProcessResult(&r);
        memset(pcm, 0, sizeof pcm);
        if (soft) {
            /* dvsisoft text: per position two hex digits of reliability and the hard bit */
            mbe_soft_bit sb[184];
            int n = imbe ? 184 : 96;
            if ((int)strlen(line) < 3 * n) continue;
            for (int i = 0; i < n; ++i) {
                unsigned rel; sscanf(line + 3 * i, "%2x", &rel);
                sb[i].reliability = (uint8_t)rel; sb[i].bit = line[3 * i + 2] == '1';
            }
            if (imbe) {
                mbe_soft_bit fr[8][23]; char d[88];
                for (int i = 0; i < 184; ++i) fr[i / 23][i % 23] = sb[i];
                mbe_processImbe7200x4400SoftFramef(pcm, &r, (const mbe_soft_bit (*)[23])fr, d, &cur, &prev, &enh);
            } else {
                mbe_soft_bit fr[4][24]; char d[49];
                for (int i = 0; i < 96; ++i) fr[i / 24][i % 24] = sb[i];
                if (dstar) mbe_processAmbe3600x2400SoftFramef(pcm, &r, (const mbe_soft_bit (*)[24])fr, d, &cur, &prev, &enh);
                else mbe_processAmbe3600x2450SoftFramef(pcm, &r, (const mbe_soft_bit (*)[24])fr, d, &cur, &prev, &enh);
            }
        } else if (imbe) {
            if (strlen(line) < 184) continue;
            char fr[8][23], d[88];
            for (int i = 0; i < 184; ++i) fr[i / 23][i % 23] = line[i] == '1';
            mbe_processImbe7200x4400Framef(pcm, &r, (const char (*)[23])fr, d, &cur, &prev, &enh);
        } else {
            if (strlen(line) < 96) continue;
            char fr[4][24], d[49];
            for (int i = 0; i < 96; ++i) fr[i / 24][i % 24] = line[i] == '1';
            if (dstar) mbe_processAmbe3600x2400Framef(pcm, &r, (const char (*)[24])fr, d, &cur, &prev, &enh);
            else mbe_processAmbe3600x2450Framef(pcm, &r, (const char (*)[24])fr, d, &cur, &prev, &enh);
        }
        fwrite(pcm, sizeof(float), 160, out);
    }
    fclose(in); fclose(out); return 0;
}
