// SPDX-License-Identifier: GPL-3.0-or-later
// Parameter-field packing: ambe_d[49] <-> quantiser indices b[0..8], exactly as the
// mbelib decoders read them.
#include "internal/config.h"
#include <mbevocoder/mbevocoder.h>

#include "internal/bits.hpp"

#include <cstring>

using namespace mbe;

namespace {

struct Field { uint8_t width; uint8_t idx[9]; };   // idx[0] is the MSB

// AMBE+2 3600x2450 (TIA-102.BABA-1 Table 2 as unpacked by mbelib). The AMBE-3000R reads the
// low bits of b3 and b4 in another order than mbelib (and JMBE): b4's three from d[40..42] and
// b3's one from d[43], where mbelib has b3's in d[40] and b4's in d[41..43] (measured,
// docs/decoder-quality.md "AMBE+2: the order of b3 and b4's low bits"). MBEVOCODER_CHIP_MEASURED
// packs and unpacks the AMBE-3000R's order, as the decoder reads it.
constexpr Field k2450[9] = {
    {7, {0, 1, 2, 3, 37, 38, 39}},
    {5, {4, 5, 6, 7, 35}},
    {5, {8, 9, 10, 11, 36}},
#if MBEVOCODER_CHIP_MEASURED
    {9, {12, 13, 14, 15, 16, 17, 18, 19, 43}},
    {7, {20, 21, 22, 23, 40, 41, 42}},
#else
    {9, {12, 13, 14, 15, 16, 17, 18, 19, 40}},
    {7, {20, 21, 22, 23, 41, 42, 43}},
#endif
    {5, {24, 25, 26, 27, 44}},
    {4, {28, 29, 30, 45}},
    {4, {31, 32, 33, 46}},
    {3, {34, 47, 48}},
};

// AMBE 3600x2400 (D-STAR). ambe_d[24] is the C1 parity bit and belongs to no field; b8 is
// read by mbelib as a 4-bit index whose LSB is always 0 (three real bits).
constexpr Field k2400[9] = {
    {7, {0, 1, 2, 3, 4, 5, 48}},
    {4, {38, 39, 40, 41}},
    {6, {6, 7, 8, 9, 42, 43}},
    {9, {10, 11, 12, 13, 14, 15, 16, 44, 45}},
    {7, {17, 18, 19, 20, 21, 46, 47}},
    {4, {22, 23, 25, 26}},
    {4, {27, 28, 29, 30}},
    {4, {31, 32, 33, 34}},
    {3, {35, 36, 37}},
};

int unpack(const Field* f, const char d[49], int b[9], bool b8Shift)
{
    if (!b) return MBE_STATUS_INVALID_ARGUMENT;
    int ret = bits::validate(d, 49);
    if (ret < 0) return ret;
    for (int k = 0; k < 9; ++k) {
        int v = 0;
        for (int i = 0; i < f[k].width; ++i) v = (v << 1) | d[f[k].idx[i]];
        b[k] = v;
    }
    if (b8Shift) b[8] <<= 1;
    return 0;
}

int pack(const Field* f, const int b[9], char d[49], bool b8Shift)
{
    if (!b || !d) return MBE_STATUS_INVALID_ARGUMENT;
    for (int k = 0; k < 9; ++k) {
        int v = b[k];
        if (k == 8 && b8Shift) {
            // the D-STAR b8 index is read as 4 bits with a zero LSB: only even values exist
            if (v & 1) return MBE_STATUS_INVALID_ARGUMENT;
            v >>= 1;
        }
        if (v < 0 || v >= (1 << f[k].width)) return MBE_STATUS_INVALID_ARGUMENT;
    }
    std::memset(d, 0, 49);
    for (int k = 0; k < 9; ++k) {
        int v = b[k];
        if (k == 8 && b8Shift) v >>= 1;
        for (int i = f[k].width - 1; i >= 0; --i) { d[f[k].idx[i]] = static_cast<char>(v & 1); v >>= 1; }
    }
    return 0;
}

} // namespace

extern "C" {

int mbe_unpackAmbe2450Fields(const char d[49], int b[9]) { return unpack(k2450, d, b, false); }
int mbe_packAmbe2450Fields(const int b[9], char d[49]) { return pack(k2450, b, d, false); }
int mbe_unpackAmbe2400Fields(const char d[49], int b[9]) { return unpack(k2400, d, b, true); }
int mbe_packAmbe2400Fields(const int b[9], char d[49]) { return pack(k2400, b, d, true); }

// mbelib-neo 2.4's frame classifier (TIA-102.BABA-1 4.1 Table 4 and 7): a tone frame is recognised
// by u0's first six bits all 1, before b0 is read, and is an erasure unless its index is one the
// decoder can play (Table 9, 255 the silent tone) and its redundant fields agree (JMBE's check: u3's
// last four bits 0, or u1's two copies of ID(7..4) equal); otherwise b0 gives the type.
int mbe_classifyAmbe2450Frame(const char d[49])
{
    int ret = mbe::bits::validate(d, 49);
    if (ret < 0) return ret;
    bool tone = true;
    for (int i = 0; i < 6; ++i) tone = tone && d[i] == 1;
    if (tone) {
        unsigned u1 = 0, u3 = 0, id = 0;
        for (int i = 12; i < 24; ++i) u1 = (u1 << 1) | static_cast<unsigned>(d[i]);
        for (int i = 35; i < 49; ++i) u3 = (u3 << 1) | static_cast<unsigned>(d[i]);
        for (int i = 12; i < 20; ++i) id = (id << 1) | static_cast<unsigned>(d[i]);
        const bool consistent = (u3 & 0xfu) == 0 || ((u1 >> 8) & 0xfu) == (u1 & 0xfu);
        const bool usable = id == 255 || mbe_toneFrequencies(static_cast<int>(id), nullptr, nullptr);
        return consistent && usable ? MBE_AMBE2450_FRAME_TONE : MBE_AMBE2450_FRAME_ERASURE;
    }
    const int b0 = (d[0] << 6) | (d[1] << 5) | (d[2] << 4) | (d[3] << 3) | (d[37] << 2) | (d[38] << 1) | d[39];
    if (b0 < 120) return MBE_AMBE2450_FRAME_VOICE;
    if (b0 == 124 || b0 == 125) return MBE_AMBE2450_FRAME_SILENCE;
    return MBE_AMBE2450_FRAME_ERASURE;
}

} // extern "C"
