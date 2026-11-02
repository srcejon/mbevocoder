// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/imbe_vocoder/typedef.h"
#include "analysis/imbe_vocoder/basic_op.h"
#include "test_util.hpp"

int main()
{
    // Every signed 16-bit input, especially the negative half and INT16_MIN.
    for (int n = -32768; n <= 32767; ++n)
        CHECK_EQ(L_deposit_h(static_cast<Word16>(n)), static_cast<int64_t>(n) * 65536);

    const Word32 inputs[] = {MIN_32, MIN_32 + 1, -65536, -1, 0, 1, 65536, MAX_32 - 1, MAX_32};
    for (Word32 a : inputs) for (Word32 b : inputs) {
        for (int subtract = 0; subtract < 2; ++subtract) {
            int64_t wide = subtract ? static_cast<int64_t>(a) - b : static_cast<int64_t>(a) + b;
            const bool overflow = wide < MIN_32 || wide > MAX_32;
            const int64_t expected = wide < MIN_32 ? MIN_32 : wide > MAX_32 ? MAX_32 : wide;
            Overflow = 0;
            CHECK_EQ(subtract ? L_sub(a, b) : L_add(a, b), expected);
            CHECK_EQ(Overflow, overflow ? 1 : 0);
        }
    }
    for (Word16 shift : {Word16(16), Word16(31), Word16(32), Word16(32767)}) {
        CHECK_EQ(shl(1, shift), MAX_16);
        CHECK_EQ(shl(-1, shift), -32768);
        CHECK_EQ(shl(0, shift), 0);
    }
    Carry = 1; Overflow = 0;
    CHECK_EQ(L_add_c(MAX_32, 0), MIN_32);
    CHECK_EQ(Overflow, 1);
    Carry = 0; Overflow = 0;
    CHECK_EQ(L_sub_c(MIN_32, 0), MAX_32);
    CHECK_EQ(Overflow, 1);
    return test::finish("test_fixedpoint");
}
