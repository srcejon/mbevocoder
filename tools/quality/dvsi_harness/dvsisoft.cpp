// SPDX-License-Identifier: GPL-3.0-or-later
// DVSI soft-decision vectors (4-bit soft values, two per byte, high nibble first, 0 = strong 0,
// 15 = strong 1, in the order of the hard file's bits) -> mbevocoder soft decode (raw float32), and a
// text dump of the soft frame (per ambe_fr/imbe_fr position two hex digits of
// reliability and the hard bit) for neodec.
#include <mbevocoder/mbevocoder.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: dvsisoft dstar|ambe2450|imbe7200 in.sd out.f32 out.txt\n"); return 2; }
    const bool dstar = !std::strcmp(argv[1], "dstar"), imbe = !std::strcmp(argv[1], "imbe7200");
    FILE* in = std::fopen(argv[2], "rb");
    FILE* out = std::fopen(argv[3], "wb");
    FILE* txt = std::fopen(argv[4], "w");
    if (!in || !out || !txt) return 1;
    std::vector<unsigned char> raw;
    for (int c; (c = std::fgetc(in)) != EOF;) raw.push_back((unsigned char)c);
    std::vector<unsigned char> nib;
    for (unsigned char b : raw) { nib.push_back(b >> 4); nib.push_back(b & 15); }
    const size_t nbits = imbe ? 144 : 72;
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    for (size_t f = 0; (f + 1) * nbits <= nib.size(); ++f) {
        mbe_soft_bit bits[144];
        for (size_t t = 0; t < nbits; ++t) {
            // on-air bit t: MSB first in each byte for r33/p25, LSB first for D-STAR
            const size_t byte = t / 8, k = t % 8;
            const int v = nib[f * nbits + byte * 8 + (dstar ? 7 - k : k)];
            bits[t].bit = (uint8_t)(v >= 8);
            bits[t].reliability = (uint8_t)(std::abs(2 * v - 15) * 17);
        }
        float pcm[160];
        mbe_process_result r;
        mbe_initProcessResult(&r);
        int st;
        if (imbe) {
            mbe_soft_bit fr[8][23];
            char d[88];
            mbe_imbeSoftFrameFromBits(MBE_LAYOUT_P25P1, bits, fr);
            for (int i = 0; i < 184; ++i) { const mbe_soft_bit& s = fr[i / 23][i % 23]; std::fprintf(txt, "%02x%c", s.reliability, s.bit ? '1' : '0'); }
            st = mbe_processImbe7200x4400SoftFramef(pcm, &r, fr, d, &cur, &prev, &enh);
        } else {
            mbe_soft_bit fr[4][24];
            char d[49];
            mbe_ambeSoftFrameFromBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits, fr);
            st = dstar ? mbe_processAmbe3600x2400SoftFramef(pcm, &r, fr, d, &cur, &prev, &enh)
                       : mbe_processAmbe3600x2450SoftFramef(pcm, &r, fr, d, &cur, &prev, &enh);
            for (int i = 0; i < 96; ++i) {
                const mbe_soft_bit& s = fr[i / 24][i % 24];
                std::fprintf(txt, "%02x%c", s.reliability, s.bit ? '1' : '0');
            }
        }
        if (st < 0) std::memset(pcm, 0, sizeof pcm);
        std::fwrite(pcm, sizeof(float), 160, out);
        std::fputc('\n', txt);
    }
    return 0;
}
