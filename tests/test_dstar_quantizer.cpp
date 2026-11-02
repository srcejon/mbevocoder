// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoder/ambe_quant.hpp"
#include "encoder/dstar_quantizer.hpp"
#include "test_util.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

static double decodedShapeError(const mbe::AmbeRateTables& tables, const int b[9],
                                const float* target, const mbe_parms& previous)
{
    char bits[49] = {};
    mbe_parms current{}, prev = previous;
    CHECK_EQ(tables.packFields(b, bits), 0);
    CHECK_EQ(tables.decodeParms(bits, &current, &prev), 0);
    float meanTarget = 0, meanDecoded = 0;
    for (int l = 0; l < current.L; ++l) {
        meanTarget += target[l];
        meanDecoded += current.log2Ml[l + 1];
    }
    meanTarget /= static_cast<float>(current.L);
    meanDecoded /= static_cast<float>(current.L);
    double error = 0;
    for (int l = 0; l < current.L; ++l) {
        const float weight = std::clamp(std::exp2(0.5f * (target[l] - meanTarget)), 0.1f, 10.0f);
        const double difference = (current.log2Ml[l + 1] - meanDecoded) - (target[l] - meanTarget);
        error += weight * difference * difference;
    }
    return error;
}

int main()
{
    const auto& tables = mbe::ambe2400Tables();
    test::Rng rng(731);
    int improved = 0;
    mbe::AmbeQuantizer quantizer(tables);
    // Every D-STAR pitch index, through a continuous changing-length predictor.
    // Score with the actual decoder, independently of the search's basis/matrix.
    for (int b0 = 0; b0 < 120; ++b0) {
        for (int shape = 0; shape < 3; ++shape) {
            const int L = static_cast<int>(tables.Ltable[b0]);
            float magnitudes[57] = {}, target[56], residual[56];
            int voiced[57] = {};
            for (int l = 1; l <= L; ++l) {
                float logAmplitude = 4.0f + 2.0f * std::sin(static_cast<float>(l) * 0.31f)
                                     + static_cast<float>(rng.below(100)) * 0.01f;
                if (shape == 1) logAmplitude -= static_cast<float>(l) * 0.2f;
                if (shape == 2 && l % 4 == 0) logAmplitude -= 12.0f;
                magnitudes[l] = std::exp2(logAmplitude);
                voiced[l] = 1;
                target[l - 1] = std::log2(magnitudes[l]) + 0.5f * std::log2(static_cast<float>(L));
            }
            const mbe_parms previous = quantizer.current();
            int before[9], after[9];
            CHECK(quantizer.quantizeModel(2.0f * 3.14159265358979f * tables.f0(b0), L, voiced, magnitudes, 0.0f, before));
            CHECK_EQ(before[0], b0);
            std::memcpy(after, before, sizeof after);
            for (int l = 0; l < L; ++l) {
                const float position = static_cast<float>(previous.L) / static_cast<float>(L) * static_cast<float>(l + 1);
                const int lower = static_cast<int>(position);
                const int upper = std::min(lower + 1, 56);
                const float fraction = position - static_cast<float>(lower);
                const float low = previous.log2Ml[lower == 0 ? 1 : lower];
                residual[l] = target[l] - tables.pred * (1.0f - fraction) * low
                              - tables.pred * fraction * previous.log2Ml[upper];
            }
            mbe::refineDstarShape(tables, L, residual, target, after);
            for (int k = 0; k < 3; ++k) CHECK_EQ(before[k], after[k]);
            CHECK(after[3] >= 0 && after[3] < 512);
            CHECK(after[4] >= 0 && after[4] < 128);
            for (int k = 0; k < 4; ++k) CHECK(after[k + 5] >= 0 && after[k + 5] < tables.hocRows[k]);
            CHECK_EQ(after[8] % 2, 0);
            const double original = decodedShapeError(tables, before, target, previous);
            const double refined = decodedShapeError(tables, after, target, previous);
            CHECK(refined <= original + 1e-4 * (1.0 + original));
            if (refined < original - 1e-3) ++improved;
        }
    }
    CHECK(improved > 100);
    return test::finish("test_dstar_quantizer");
}
