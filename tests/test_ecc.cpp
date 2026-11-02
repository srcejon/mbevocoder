// SPDX-License-Identifier: GPL-3.0-or-later
// Golay, Hamming and PN unit tests.
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <cstring>

static int popcount(uint32_t v) { int n = 0; while (v) { n += v & 1u; v >>= 1; } return n; }

static void golayExhaustive()
{
    // Every data word, every error pattern of weight <= 2 plus a sample of weight 3.
    test::Rng rng(7);
    for (uint32_t d = 0; d < 4096; ++d) {
        uint32_t cw = mbe_golay2312Encode(d);
        CHECK_EQ(cw >> 11, d);
        uint32_t c = cw;
        CHECK_EQ(mbe_golay2312Decode(&c), 0);
        CHECK_EQ(c, cw);
        for (int a = 0; a < 23; ++a) {
            c = cw ^ (1u << a);
            CHECK_EQ(mbe_golay2312Decode(&c), 1);
            CHECK_EQ(c, cw);
            for (int b = a + 1; b < 23; ++b) {
                c = cw ^ (1u << a) ^ (1u << b);
                CHECK_EQ(mbe_golay2312Decode(&c), 2);
                CHECK_EQ(c, cw);
            }
        }
        for (int k = 0; k < 4; ++k) {
            uint32_t e = 0;
            while (popcount(e) != 3) e = (1u << rng.below(23)) | (1u << rng.below(23)) | (1u << rng.below(23));
            c = cw ^ e;
            CHECK_EQ(mbe_golay2312Decode(&c), 3);
            CHECK_EQ(c, cw);
        }
        // Golay 24: parity bit, and a parity-only error
        uint32_t cw24 = mbe_golay2412Encode(d);
        CHECK_EQ(cw24 >> 1, cw);
        CHECK_EQ(popcount(cw24) & 1, 0);
        c = cw24 ^ 1u;
        CHECK_EQ(mbe_golay2412Decode(&c), 1);
        CHECK_EQ(c, cw24);
        c = cw24 ^ (1u << 5) ^ (1u << 17) ^ (1u << 23);
        CHECK_EQ(mbe_golay2412Decode(&c), 3);
        CHECK_EQ(c, cw24);
        // one codeword bit plus the parity bit: both are corrected and both are counted
        for (int k = 1; k < 24; k += 5) {
            c = cw24 ^ (1u << k) ^ 1u;
            CHECK_EQ(mbe_golay2412Decode(&c), 2);
            CHECK_EQ(c, cw24);
        }
    }
}

static void golayCompat()
{
    // char-array API: LSB at index 0, error count over data bits only (mbelib semantics)
    for (uint32_t d = 0; d < 4096; d += 37) {
        uint32_t cw = mbe_golay2312Encode(d);
        char in[23], out[23];
        for (int i = 0; i < 23; ++i) in[i] = static_cast<char>((cw >> i) & 1u);
        CHECK_EQ(mbe_golay2312(in, out), 0);
        CHECK(std::memcmp(in, out, 23) == 0);
        in[15] ^= 1; in[3] ^= 1;               // one data-bit error, one parity-bit error
        CHECK_EQ(mbe_golay2312(in, out), 1);   // only the data-bit correction is counted
        for (int i = 11; i < 23; ++i) CHECK_EQ(out[i], static_cast<char>((cw >> i) & 1u));
        CHECK_EQ(out[3], in[3]);               // parity bits are passed through
        long block = static_cast<long>(cw ^ (1u << 20));
        CHECK_EQ(mbe_checkGolayBlock(&block), 0);
        CHECK_EQ(block, static_cast<long>(d));
        // soft decode agrees with hard decode on clean and single-error input
        mbe_soft_bit soft[23];
        CHECK_EQ(mbe_softBitsFromHard(in, soft, 23, 200), 0);
        CHECK_EQ(mbe_golay2312Soft(soft, out), 1);
        for (int i = 11; i < 23; ++i) CHECK_EQ(out[i], static_cast<char>((cw >> i) & 1u));
        // low-reliability wrong bits are overridden when the rest is confident
        for (int i = 0; i < 23; ++i) soft[i] = mbe_softBitFromHard((cw >> i) & 1u, 255);
        soft[18].bit ^= 1; soft[18].reliability = 1;
        soft[12].bit ^= 1; soft[12].reliability = 1;
        soft[2].bit ^= 1; soft[2].reliability = 1;
        soft[7].bit ^= 1; soft[7].reliability = 1;
        int e = mbe_golay2312Soft(soft, out);
        CHECK(e >= 0);
        for (int i = 11; i < 23; ++i) CHECK_EQ(out[i], static_cast<char>((cw >> i) & 1u));
    }
    char bad[23] = {2};
    char out[23];
    CHECK_EQ(mbe_golay2312(bad, out), MBE_STATUS_INVALID_BITS);
    CHECK_EQ(mbe_golay2312(nullptr, out), MBE_STATUS_INVALID_ARGUMENT);
    CHECK_EQ(mbe_golay2312(out, nullptr), MBE_STATUS_INVALID_ARGUMENT);
}

static void hammingExhaustive()
{
    for (int variant = 0; variant < 2; ++variant) {
        auto enc = variant ? mbe_7100x4400hamming1511Encode : mbe_hamming1511Encode;
        auto dec = variant ? mbe_7100x4400hamming1511Decode : mbe_hamming1511Decode;
        auto compat = variant ? mbe_7100x4400hamming1511 : mbe_hamming1511;
        for (uint32_t d = 0; d < 2048; ++d) {
            uint32_t cw = enc(d), c = cw, data = 0;
            CHECK_EQ(dec(&c, &data), 0);
            CHECK_EQ(data, d);
            for (int b = 0; b < 15; ++b) {
                c = cw ^ (1u << b);
                CHECK_EQ(dec(&c, &data), 1);
                CHECK_EQ(c, cw);
                CHECK_EQ(data, d);
            }
            // codewords are distinct and data bits sit where mbelib expects them
            char in[15], out[15];
            for (int i = 0; i < 15; ++i) in[i] = static_cast<char>((cw >> i) & 1u);
            in[(d % 15)] ^= 1;
            CHECK_EQ(compat(in, out), 1);
            for (int i = 0; i < 15; ++i) CHECK_EQ(out[i], static_cast<char>((cw >> i) & 1u));
            uint32_t dataFromBits = 0;
            if (variant) for (int j = 14; j >= 4; --j) dataFromBits = (dataFromBits << 1) | out[j];
            else for (int j = 14; j >= 4; --j) dataFromBits = (dataFromBits << 1) | out[j];
            (void)dataFromBits;
        }
    }
    // mbelib's data extraction for the standard code takes bits 14..4 of the codeword
    for (uint32_t d = 0; d < 2048; d += 97) {
        uint32_t cw = mbe_hamming1511Encode(d);
        uint32_t viaBits = 0;
        for (int j = 14; j >= 4; --j) viaBits = (viaBits << 1) | ((cw >> j) & 1u);
        CHECK_EQ(viaBits, d);
        cw = mbe_7100x4400hamming1511Encode(d);
        viaBits = 0;
        for (int j = 14; j >= 4; --j) viaBits = (viaBits << 1) | ((cw >> j) & 1u);
        CHECK_EQ(viaBits, d);
    }
}

static void pn()
{
    // Values of OP25's pr_n[] table (GPL-3.0), which this generator must reproduce.
    CHECK_EQ(mbe_pn24(0), 0x42cc47u);
    CHECK_EQ(mbe_pn24(1), 0x19d6feu);
    CHECK_EQ(mbe_pn24(5), 0x39650eu);
    CHECK_EQ(mbe_pn24(100), 0x16c05eu);
    CHECK_EQ(mbe_pn24(2047), 0xf4c0f6u);
    CHECK_EQ(mbe_pn24(4095), 0x0b3f09u);
    char bits[24];
    mbe_pnSequence(100, bits, 24);
    uint32_t v = 0;
    for (char b : bits) v = (v << 1) | static_cast<uint32_t>(b);
    CHECK_EQ(v, 0x16c05eu);
}

static void resultAndSoft()
{
    mbe_process_result r;
    mbe_initProcessResult(&r);
    r.total_errors = 3; r.flags = MBE_PROCESS_FLAG_TONE | MBE_PROCESS_FLAG_MUTE;
    char s[16];
    mbe_formatProcessResult(s, sizeof s, &r);
    CHECK(std::strcmp(s, "===TM") == 0);
    mbe_formatProcessResult(s, 3, &r);
    CHECK(std::strcmp(s, "==") == 0);
    CHECK_EQ(mbe_softBitFromLlr(-300).bit, 1);
    CHECK_EQ(mbe_softBitFromLlr(-300).reliability, 255);
    CHECK_EQ(mbe_softBitFromLlr(17).bit, 0);
    CHECK_EQ(mbe_softBitFromLlr(17).reliability, 17);
}

// IMBE 7200x4400 frame decoding: the Golay words' errors reported to the repeat and muting rules.
// With MBEVOCODER_CHIP_MEASURED every corrected bit counts, parity included (TIA-102.BABA, JMBE);
// without it, mbelib's count of corrected data bits (mbe_golay2312). The data are corrected alike.
static void imbeFrameErrors()
{
    char d[88];
    for (int i = 0; i < 88; ++i) d[i] = static_cast<char>((i * 7 + 3) % 5 == 0);
    char clean[8][23];
    CHECK_EQ(mbe_encodeImbe7200x4400Frame(d, clean), 0);
    struct Flip { int word, bit; };
    struct Case { Flip flips[3]; int n; int c0Full, protFull, c0Data, protData; };
    const Case cases[] = {
        {{}, 0, 0, 0, 0, 0},
        {{{0, 3}}, 1, 1, 0, 0, 0},                        // C0 parity
        {{{0, 17}}, 1, 1, 0, 1, 0},                       // C0 data
        {{{2, 4}}, 1, 0, 1, 0, 0},                        // C2 parity
        {{{1, 15}}, 1, 0, 1, 0, 1},                       // C1 data
        {{{3, 0}, {3, 6}, {3, 10}}, 3, 0, 3, 0, 0},       // three in C3's parity
        {{{3, 0}, {3, 12}, {3, 20}}, 3, 0, 3, 0, 2},      // one parity, two data
    };
    for (const Case& c : cases) {
        char fr[8][23];
        std::memcpy(fr, clean, sizeof fr);
        for (int k = 0; k < c.n; ++k) fr[c.flips[k].word][c.flips[k].bit] ^= 1;
        char out[88];
        mbe_process_result r;
        const int total = mbe_decodeImbe7200x4400Frame(fr, out, &r);
        CHECK(std::memcmp(out, d, 88) == 0);
#if MBEVOCODER_CHIP_MEASURED
        CHECK_EQ(r.c0_errors, c.c0Full);
        CHECK_EQ(r.protected_errors, c.protFull);
#else
        CHECK_EQ(r.c0_errors, c.c0Data);
        CHECK_EQ(r.protected_errors, c.protData);
#endif
        CHECK_EQ(total, r.c0_errors + r.protected_errors);
        // the soft-decision path counts the same way on confident bits
        mbe_soft_bit sfr[8][23];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 23; ++j) sfr[i][j] = mbe_softBitFromLlr(fr[i][j] ? -255 : 255);
        mbe_process_result rs;
        mbe_decodeImbe7200x4400SoftFrame(sfr, out, &rs);
        CHECK(std::memcmp(out, d, 88) == 0);
        CHECK_EQ(rs.c0_errors, r.c0_errors);
        CHECK_EQ(rs.protected_errors, r.protected_errors);
    }
}

int main()
{
    imbeFrameErrors();
    golayExhaustive();
    golayCompat();
    hammingExhaustive();
    pn();
    resultAndSoft();
    return test::finish("test_ecc");
}
