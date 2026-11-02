// SPDX-License-Identifier: GPL-3.0-or-later
// AMBE tone frames: the inverse of the decoders' tone parsing (src/decoder/ambe3600x2450.c,
// ambe3600x2400.c, mbelib.c). Verified against the AMBE-3000R: see docs/tone-frames.md.
#include <mbevocoder/mbevocoder.h>

#include "encoder/encoder_internal.hpp"

#include <cmath>
#include <cstring>

namespace {

struct Dual { float f1, f2; };
// Dual-tone table indexed by tone_id - 128 (same as the decoder's mbe_tone.h).
const Dual kDual[36] = {
    {1336.0f, 941.0f}, {1209.0f, 697.0f}, {1336.0f, 697.0f}, {1477.0f, 697.0f}, {1209.0f, 770.0f},
    {1336.0f, 770.0f}, {1477.0f, 770.0f}, {1209.0f, 852.0f}, {1336.0f, 852.0f}, {1477.0f, 852.0f},
    {1633.0f, 697.0f}, {1633.0f, 770.0f}, {1633.0f, 852.0f}, {1633.0f, 941.0f}, {1209.0f, 941.0f},
    {1477.0f, 941.0f}, {1162.0f, 820.0f}, {1052.0f, 606.0f}, {1162.0f, 606.0f}, {1279.0f, 606.0f},
    {1052.0f, 672.0f}, {1162.0f, 672.0f}, {1279.0f, 672.0f}, {1052.0f, 743.0f}, {1162.0f, 743.0f},
    {1279.0f, 743.0f}, {1430.0f, 606.0f}, {1430.0f, 672.0f}, {1430.0f, 743.0f}, {1430.0f, 820.0f},
    {1052.0f, 820.0f}, {1279.0f, 820.0f}, {440.0f, 350.0f},  {480.0f, 440.0f},  {620.0f, 480.0f},
    {490.0f, 350.0f},
};

void putBits(char* d, int first, int count, int value)   // MSB first
{
    for (int i = 0; i < count; ++i) d[first + i] = static_cast<char>((value >> (count - 1 - i)) & 1);
}

} // namespace

extern "C" {

int mbe_toneFrequencies(int tone_id, float* f1, float* f2)
{
    float a = 0, b = 0;
    int ok = 1;
    if (tone_id == 5) a = b = 156.25f;
    else if (tone_id == 6) a = b = 187.5f;
    else if (tone_id >= 7 && tone_id <= 122) a = b = 31.25f * static_cast<float>(tone_id);
    else if (tone_id >= 128 && tone_id <= 163) { a = kDual[tone_id - 128].f1; b = kDual[tone_id - 128].f2; }
    else ok = 0;
    if (f1) *f1 = a;
    if (f2) *f2 = b;
    return ok;
}

int mbe_toneIdFromFrequencies(float f1, float f2)
{
    if (!std::isfinite(f1) || !std::isfinite(f2)) return -1;   // NaN passes every range test below
    if (f2 > 0.0f && std::fabs(f2 - f1) > 1.0f) {
        int best = -1; float bestErr = 0;
        for (int i = 0; i < 36; ++i) {
            float e1 = std::fabs(kDual[i].f1 - f1) + std::fabs(kDual[i].f2 - f2);
            float e2 = std::fabs(kDual[i].f1 - f2) + std::fabs(kDual[i].f2 - f1);
            float e = e1 < e2 ? e1 : e2;
            if (best < 0 || e < bestErr) { best = i; bestErr = e; }
        }
        return bestErr <= 20.0f ? 128 + best : -1;
    }
    if (f1 < 140.0f || f1 > 3830.0f) return -1;
    if (f1 < 172.0f) return 5;
    if (f1 < 203.0f) return 6;
    int id = static_cast<int>(f1 / 31.25f + 0.5f);
    if (id < 7) id = 7;
    if (id > 122) id = 122;
    return id;
}

int mbe_encodeAmbe2450ToneData(int tone_id, int amplitude, char ambe_d[49])
{
    if (!ambe_d || !mbe_toneFrequencies(tone_id, nullptr, nullptr) || amplitude < 0 || amplitude > 127)
        return MBE_STATUS_INVALID_ARGUMENT;
    // u0 (12): 111111 | AD[6:1];  u1 (12): ID | ID[7:4];  u2 (11): ID[3:0] | ID[7:1];
    // u3 (14): ID[0] | ID | AD[0] | 0000.  The identifier is repeated four times.
    const int id = tone_id & 0xff;
    const int u0 = (63 << 6) | ((amplitude >> 1) & 0x3f);
    const int u1 = (id << 4) | (id >> 4);
    const int u2 = ((id & 0xf) << 7) | (id >> 1);
    const int u3 = ((id & 1) << 13) | (id << 5) | ((amplitude & 1) << 4);
    putBits(ambe_d, 0, 12, u0);
    putBits(ambe_d, 12, 12, u1);
    putBits(ambe_d, 24, 11, u2);
    putBits(ambe_d, 35, 14, u3);
    return 0;
}

int mbe_toneIdToDstar(int tone_id)
{
    if (tone_id >= 5 && tone_id <= 122) return tone_id;
    float f1, f2;
    if (!mbe_toneFrequencies(tone_id, &f1, &f2)) return -1;
    static const float cols[4] = {1209.0f, 1336.0f, 1477.0f, 1633.0f};
    static const float rows[4] = {697.0f, 770.0f, 852.0f, 941.0f};
    if (tone_id >= 128 && tone_id <= 143) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (cols[c] == f1 && rows[r] == f2) return 128 + 4 * c + r;
        return -1;
    }
    if (tone_id >= 160 && tone_id <= 163) return 144 + (tone_id - 160);   // dial, ring, busy, reorder
    return -1;   // KNOX tones have no D-STAR representation
}

int mbe_toneIdFromDstar(int dstar_id)
{
    if (dstar_id >= 5 && dstar_id <= 122) return dstar_id;
    if (dstar_id >= 128 && dstar_id <= 143) {
        static const float cols[4] = {1209.0f, 1336.0f, 1477.0f, 1633.0f};
        static const float rows[4] = {697.0f, 770.0f, 852.0f, 941.0f};
        float f1 = cols[(dstar_id - 128) >> 2], f2 = rows[(dstar_id - 128) & 3];
        for (int i = 0; i < 16; ++i)
            if (kDual[i].f1 == f1 && kDual[i].f2 == f2) return 128 + i;
        return -1;
    }
    if (dstar_id >= 144 && dstar_id <= 147) return 160 + (dstar_id - 144);
    return -1;
}

int mbe_encodeAmbe2400ToneData(int tone_id, int amplitude, char ambe_d[49])
{
    if (!ambe_d || amplitude < 0 || amplitude > 127) return MBE_STATUS_INVALID_ARGUMENT;
    const int id = mbe_toneIdToDstar(tone_id);
    if (id < 0) return mbe_toneFrequencies(tone_id, nullptr, nullptr) ? MBE_STATUS_ENCODE_RANGE : MBE_STATUS_INVALID_ARGUMENT;
    // Layout as the decoder reads it (AMBE-3000R-verified, docs/tone-frames.md): b0 = 126 in
    // d[0..5],d[48]; tone index bits 7..5 through a 3-bit code in d[6..8], bit 4 in d[9],
    // bits 3,2 in d[42],d[43], bits 1,0 in d[10],d[11]; the 8-bit volume in d[12..16],
    // d[44], d[45], d[17]. The AMBE-3000R's volume is 2 AD - 17 on the AMBE+2 amplitude scale.
    static const int t7[8] = {1, 0, 0, 0, 0, 1, 1, 1};
    static const int t6[8] = {0, 0, 0, 1, 1, 1, 1, 0};
    static const int t5[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    std::memset(ambe_d, 0, 49);
    const int top = id >> 5;
    int def = 0;
    for (int i = 0; i < 8; ++i)
        if (t7[i] == ((top >> 2) & 1) && t6[i] == ((top >> 1) & 1) && t5[i] == (top & 1)) { def = i; break; }
    putBits(ambe_d, 0, 6, 126 >> 1);
    ambe_d[48] = 0;
    putBits(ambe_d, 6, 3, def);
    ambe_d[9] = static_cast<char>((id >> 4) & 1);
    ambe_d[42] = static_cast<char>((id >> 3) & 1);
    ambe_d[43] = static_cast<char>((id >> 2) & 1);
    ambe_d[10] = static_cast<char>((id >> 1) & 1);
    ambe_d[11] = static_cast<char>(id & 1);
    int vol = 2 * amplitude - 17;
    if (vol < 0) vol = 0;
    putBits(ambe_d, 12, 5, vol >> 3);
    ambe_d[44] = static_cast<char>((vol >> 2) & 1);
    ambe_d[45] = static_cast<char>((vol >> 1) & 1);
    ambe_d[17] = static_cast<char>(vol & 1);
    return 0;
}

int mbe_encoderTone(mbe_encoder* e, int tone_id, int amplitude, mbe_layout layout, char* bits)
{
    if (!e || !bits) return MBE_STATUS_INVALID_ARGUMENT;
    if (mbe_layoutBits(layout) != 72 || !mbe::layoutMatchesRate(mbe_encoderRate(e), layout)) return MBE_STATUS_INVALID_ARGUMENT;
    char d[49], fr[4][24];
    int ret;
    switch (mbe_encoderRate(e)) {
        case MBE_RATE_AMBE3600x2400:
            ret = mbe_encodeAmbe2400ToneData(tone_id, amplitude, d);
            if (ret < 0) return ret;
            ret = mbe_encodeAmbe3600x2400Frame(d, fr);
            break;
        case MBE_RATE_AMBE3600x2450:
        case MBE_RATE_AMBE2450:
            ret = mbe_encodeAmbe2450ToneData(tone_id, amplitude, d);
            if (ret < 0) return ret;
            ret = mbe_encodeAmbe3600x2450Frame(d, fr);
            break;
        default:
            return MBE_STATUS_INVALID_ARGUMENT;
    }
    if (ret < 0) return ret;
    return mbe_ambeFrameToBits(layout, fr, bits);
}

int mbe_encoderToneDvsi(mbe_encoder* e, int tone_id, int amplitude, uint8_t* bytes)
{
    if (!e || !bytes) return MBE_STATUS_INVALID_ARGUMENT;
    char bits[72];
    int ret;
    switch (mbe_encoderRate(e)) {
        case MBE_RATE_AMBE2450: {
            char d[49];
            ret = mbe_encodeAmbe2450ToneData(tone_id, amplitude, d);
            if (ret < 0) return ret;
            return mbe_ambe2450DataToDvsi(d, bytes);
        }
        case MBE_RATE_AMBE3600x2450:
            ret = mbe_encoderTone(e, tone_id, amplitude, MBE_LAYOUT_DMR, bits);
            if (ret < 0) return ret;
            mbe_packBits(bits, 72, bytes, 1);
            return 0;
        case MBE_RATE_AMBE3600x2400:
            ret = mbe_encoderTone(e, tone_id, amplitude, MBE_LAYOUT_DSTAR, bits);
            if (ret < 0) return ret;
            mbe_packBits(bits, 72, bytes, 0);
            return 0;
        default:
            return MBE_STATUS_INVALID_ARGUMENT;
    }
}

} // extern "C"
