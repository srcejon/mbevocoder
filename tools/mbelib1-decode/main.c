// SPDX-License-Identifier: GPL-3.0-or-later
/* mbelib 1.x decoder driver: reads ambe_fr[4][24] or imbe_fr[8][23] bit lines (mbe-cli frbits),
   writes raw float32 */
#include "mbelib.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: mbelib1-decode dstar|ambe2450|imbe7200 fr.txt out.f32\n"); return 2; }
    int dstar = strcmp(argv[1], "dstar") == 0, imbe = strcmp(argv[1], "imbe7200") == 0;
    FILE* in = fopen(argv[2], "r"); FILE* out = fopen(argv[3], "wb");
    if (!in || !out) return 1;
    mbe_parms cur, prev, enh; mbe_initMbeParms(&cur, &prev, &enh);
    char line[256];
    while (fgets(line, sizeof line, in)) {
        float pcm[160]; int errs = 0, errs2 = 0; char err[64];
        if (imbe) {
            if (strlen(line) < 184) continue;
            char fr[8][23], d[88];
            for (int i = 0; i < 184; ++i) fr[i / 23][i % 23] = line[i] == '1';
            mbe_processImbe7200x4400Framef(pcm, &errs, &errs2, err, fr, d, &cur, &prev, &enh, 3);
        } else {
            if (strlen(line) < 96) continue;
            char fr[4][24], d[49];
            for (int i = 0; i < 96; ++i) fr[i / 24][i % 24] = line[i] == '1';
            if (dstar) mbe_processAmbe3600x2400Framef(pcm, &errs, &errs2, err, fr, d, &cur, &prev, &enh, 3);
            else mbe_processAmbe3600x2450Framef(pcm, &errs, &errs2, err, fr, d, &cur, &prev, &enh, 3);
        }
        fwrite(pcm, sizeof(float), 160, out);
    }
    fclose(in); fclose(out); return 0;
}
