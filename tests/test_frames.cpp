// SPDX-License-Identifier: GPL-3.0-or-later
// Frame assembly/disassembly, interleavers and field packing round trips.
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <cstring>

static void ambeRoundTrip(bool dstar)
{
    test::Rng rng(dstar ? 1 : 2);
    auto encode = dstar ? mbe_encodeAmbe3600x2400Frame : mbe_encodeAmbe3600x2450Frame;
    auto decode = dstar ? mbe_decodeAmbe3600x2400Frame : mbe_decodeAmbe3600x2450Frame;
    auto decodeSoft = dstar ? mbe_decodeAmbe3600x2400SoftFrame : mbe_decodeAmbe3600x2450SoftFrame;
    for (int n = 0; n < 2000; ++n) {
        char d[49], fr[4][24], back[49];
        for (char& b : d) b = rng.bit();
        CHECK_EQ(encode(d, fr), 0);
        mbe_process_result r;
        CHECK_EQ(decode(fr, back, &r), 0);
        CHECK_EQ(r.total_errors, 0);
        CHECK_EQ(r.flags, MBE_PROCESS_FLAG_C0_VALID);
        if (dstar) d[24] = back[24];   // parity position, recomputed by the encoder
        CHECK(std::memcmp(d, back, 49) == 0);
        // frame -> data -> frame is exact
        char fr2[4][24];
        CHECK_EQ(encode(back, fr2), 0);
        CHECK(std::memcmp(fr, fr2, sizeof fr) == 0);

        // up to 3 errors in C0 and up to 3 in C1 are corrected; C2/C3 errors are not
        char noisy[4][24];
        std::memcpy(noisy, fr, sizeof fr);
        unsigned e0 = rng.below(4), e1 = rng.below(4);
        test::flipBits(noisy[0], 24, e0, static_cast<uint32_t>(n) * 2u);
        test::flipBits(noisy[1], 23, e1, static_cast<uint32_t>(n) * 2u + 1u);
        int errs = decode(noisy, back, &r);
        CHECK(errs >= 0);
        CHECK_EQ(r.total_errors, errs);
        CHECK(std::memcmp(d, back, 49) == 0);
        // soft decode with confident bits gives the same data
        mbe_soft_bit soft[4][24];
        CHECK_EQ(mbe_softBitsFromHard(&noisy[0][0], &soft[0][0], 96, 255), 0);
        CHECK(decodeSoft(soft, back, &r) >= 0);
        CHECK(r.flags & MBE_PROCESS_FLAG_SOFT_INPUT);
        CHECK(std::memcmp(d, back, 49) == 0);
    }
    char d[49] = {0}, fr[4][24];
    d[5] = 3;
    CHECK_EQ(encode(d, fr), MBE_STATUS_INVALID_BITS);
    CHECK_EQ(decode(fr, nullptr, nullptr), MBE_STATUS_INVALID_ARGUMENT);
}

static void imbeRoundTrip()
{
    test::Rng rng(3);
    for (int n = 0; n < 2000; ++n) {
        char d[88], fr[8][23], back[88];
        for (char& b : d) b = rng.bit();
        CHECK_EQ(mbe_encodeImbe7200x4400Frame(d, fr), 0);
        mbe_process_result r;
        CHECK_EQ(mbe_decodeImbe7200x4400Frame(fr, back, &r), 0);
        CHECK_EQ(r.flags, MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID);
        CHECK(std::memcmp(d, back, 88) == 0);
        char noisy[8][23];
        std::memcpy(noisy, fr, sizeof fr);
        for (int row = 0; row < 4; ++row) test::flipBits(noisy[row], 23, rng.below(4), static_cast<uint32_t>(n * 8 + row));
        for (int row = 4; row < 7; ++row) test::flipBits(noisy[row], 15, rng.below(2), static_cast<uint32_t>(n * 8 + row));
        int errs = mbe_decodeImbe7200x4400Frame(noisy, back, &r);
        CHECK(errs >= 0);
        CHECK(std::memcmp(d, back, 88) == 0);
        mbe_soft_bit soft[8][23];
        CHECK_EQ(mbe_softBitsFromHard(&noisy[0][0], &soft[0][0], 8 * 23, 255), 0);
        CHECK(mbe_decodeImbe7200x4400SoftFrame(soft, back, &r) >= 0);
        CHECK(std::memcmp(d, back, 88) == 0);
    }
}

static void layouts()
{
    test::Rng rng(4);
    for (int l = 0; l < MBE_LAYOUT_COUNT; ++l) {
        auto layout = static_cast<mbe_layout>(l);
        unsigned nbits = mbe_layoutBits(layout);
        CHECK(nbits == 72 || nbits == 144);
        for (int n = 0; n < 200; ++n) {
            char bits[144], back[144];
            for (unsigned i = 0; i < nbits; ++i) bits[i] = rng.bit();
            if (nbits == 72) {
                char fr[4][24];
                CHECK_EQ(mbe_ambeFrameFromBits(layout, bits, fr), 0);
                CHECK_EQ(mbe_ambeFrameToBits(layout, fr, back), 0);
                // every cell must be covered: count ones
                int ones = 0, onesBits = 0;
                for (auto& row : fr) for (char c : row) ones += c;
                for (unsigned i = 0; i < 72; ++i) onesBits += bits[i];
                CHECK_EQ(ones, onesBits);
                mbe_soft_bit sbits[72], sfr[4][24];
                mbe_softBitsFromHard(bits, sbits, 72, 9);
                CHECK_EQ(mbe_ambeSoftFrameFromBits(layout, sbits, sfr), 0);
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 24; ++c) CHECK_EQ(sfr[r][c].bit, static_cast<uint8_t>(fr[r][c]));
            } else {
                char fr[8][23];
                CHECK_EQ(mbe_imbeFrameFromBits(layout, bits, fr), 0);
                CHECK_EQ(mbe_imbeFrameToBits(layout, fr, back), 0);
                int ones = 0, onesBits = 0;
                for (auto& row : fr) for (char c : row) ones += c;
                for (unsigned i = 0; i < 144; ++i) onesBits += bits[i];
                CHECK_EQ(ones, onesBits);
            }
            CHECK(std::memcmp(bits, back, nbits) == 0);
        }
    }
    char fr[4][24], bits[72] = {0};
    CHECK_EQ(mbe_ambeFrameFromBits(MBE_LAYOUT_P25P1, bits, fr), MBE_STATUS_INVALID_ARGUMENT);
    CHECK_EQ(mbe_layoutBits(MBE_LAYOUT_COUNT), 0u);

    // YSF VD2 and DVSI raw 49
    for (int n = 0; n < 500; ++n) {
        char d[49], b104[104], back[49];
        for (char& x : d) x = rng.bit();
        CHECK_EQ(mbe_ambe2450DataToYsfVd2(d, b104), 0);
        CHECK_EQ(mbe_ysfVd2ToAmbe2450Data(b104, back), 0);
        CHECK(std::memcmp(d, back, 49) == 0);
        // one flipped bit in the triple-redundant region is voted out
        char noisy[104];
        std::memcpy(noisy, b104, 104);
        noisy[rng.below(104)] ^= 1;
        int dis = mbe_ysfVd2ToAmbe2450Data(noisy, back);
        CHECK(dis == 0 || dis == 1);
        if (dis == 1) CHECK(std::memcmp(d, back, 49) == 0);
        uint8_t bytes[7];
        CHECK_EQ(mbe_ambe2450DataToDvsi(d, bytes), 0);
        CHECK_EQ(bytes[6] & 0x7f, 0);
        CHECK_EQ(mbe_dvsiToAmbe2450Data(bytes, back), 0);
        CHECK(std::memcmp(d, back, 49) == 0);
    }

    // pack/unpack
    char b[20], back[20];
    for (char& x : b) x = rng.bit();
    uint8_t bytes[3];
    mbe_packBits(b, 20, bytes, 1);
    mbe_unpackBits(bytes, 20, back, 1);
    CHECK(std::memcmp(b, back, 20) == 0);
    CHECK_EQ(bytes[2] & 0x0f, 0);
    mbe_packBits(b, 20, bytes, 0);
    mbe_unpackBits(bytes, 20, back, 0);
    CHECK(std::memcmp(b, back, 20) == 0);
    CHECK_EQ(bytes[2] & 0xf0, 0);
}

static void fields()
{
    static const int w2450[9] = {7, 5, 5, 9, 7, 5, 4, 4, 3};
    static const int w2400[9] = {7, 4, 6, 9, 7, 4, 4, 4, 3};
    test::Rng rng(5);
    for (int rate = 0; rate < 2; ++rate) {
        const int* w = rate ? w2400 : w2450;
        auto pack = rate ? mbe_packAmbe2400Fields : mbe_packAmbe2450Fields;
        auto unpack = rate ? mbe_unpackAmbe2400Fields : mbe_unpackAmbe2450Fields;
        for (int k = 0; k < 9; ++k) {
            for (int v = 0; v < (1 << w[k]); ++v) {
                int b[9], back[9];
                for (int j = 0; j < 9; ++j) b[j] = static_cast<int>(rng.below(1u << w[j]));
                b[k] = v;
                if (rate) b[8] <<= 1;   // 2400 b8 is a 4-bit index with a zero LSB
                char d[49];
                CHECK_EQ(pack(b, d), 0);
                CHECK_EQ(unpack(d, back), 0);
                CHECK(std::memcmp(b, back, sizeof b) == 0);
                if (rate) CHECK_EQ(d[24], 0);
            }
        }
        // every data bit belongs to exactly one field
        int b[9] = {0};
        char d[49];
        int covered = 0;
        for (int k = 0; k < 9; ++k) {
            b[k] = (1 << w[k]) - 1;
            if (rate && k == 8) b[k] <<= 1;
            CHECK_EQ(pack(b, d), 0);
            for (char c : d) covered += c;
            b[k] = 0;
        }
        CHECK_EQ(covered, rate ? 48 : 49);
        // values that cannot round trip are rejected rather than silently truncated
        for (int k = 0; k < 9; ++k) {
            int bad[9] = {0};
            bad[k] = 1 << w[k];
            if (rate && k == 8) bad[k] <<= 1;
            CHECK_EQ(pack(bad, d), MBE_STATUS_INVALID_ARGUMENT);
            bad[k] = -1;
            CHECK_EQ(pack(bad, d), MBE_STATUS_INVALID_ARGUMENT);
        }
        if (rate) {
            int odd[9] = {0};
            odd[8] = 1;
            CHECK_EQ(pack(odd, d), MBE_STATUS_INVALID_ARGUMENT);
            odd[8] = 15;
            CHECK_EQ(pack(odd, d), MBE_STATUS_INVALID_ARGUMENT);
            odd[8] = 14;
            int back[9];
            CHECK_EQ(pack(odd, d), 0);
            CHECK_EQ(unpack(d, back), 0);
            CHECK_EQ(back[8], 14);
        }
        int bad[9] = {0};
        bad[3] = 512;
        CHECK_EQ(pack(bad, d), MBE_STATUS_INVALID_ARGUMENT);
    }
}

int main()
{
    ambeRoundTrip(true);
    ambeRoundTrip(false);
    imbeRoundTrip();
    layouts();
    fields();
    return test::finish("test_frames");
}
