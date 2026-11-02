// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoder/dstar_quantizer.hpp"
#include "encoder/ambe_quant.hpp"

#include <cmath>

namespace mbe {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int kCoefficients = 23; // 7 PRBA + 4 HOC coefficients in each of 4 blocks
constexpr int kHarmonics = 56;

float codeword(const AmbeRateTables& tables, int group, int row, int component)
{
    if (group == 0) return tables.prba24[row][component];
    if (group == 1) return tables.prba58[row][component];
    return tables.hoc[group - 2][row][component];
}
} // namespace

void refineDstarShape(const AmbeRateTables& tables, int L, const float* residual,
                      const float* target, int b[9])
{
    const float count = static_cast<float>(L);
    const float sqrt2 = std::sqrt(2.0f);
    const int* lengths = tables.lmprbl[L];
    float basis[kCoefficients][kHarmonics] = {};
    int offset = 0;
    for (int block = 0; block < 4; ++block) {
        const float blockLength = static_cast<float>(lengths[block]);
        for (int j = 0; j < lengths[block]; ++j) {
            const float position = static_cast<float>(j) + 0.5f;
            const float c1 = std::cos(kPi * position / blockLength);
            for (int k = 1; k <= 7; ++k) {
                const float r0 = 2.0f * std::cos(kPi * static_cast<float>(k) * (2.0f * static_cast<float>(block) + 0.5f) / 8.0f);
                const float r1 = 2.0f * std::cos(kPi * static_cast<float>(k) * (2.0f * static_cast<float>(block) + 1.5f) / 8.0f);
                basis[k - 1][offset + j] = 0.5f * (r0 + r1) + (r0 - r1) / sqrt2 * c1;
            }
            for (int k = 2; k < lengths[block] && k < 6; ++k)
                basis[7 + block * 4 + k - 2][offset + j] =
                    2.0f * std::cos(kPi * static_cast<float>(k) * position / blockLength);
        }
        offset += lengths[block];
    }

    // Decoder normalisation removes the arithmetic mean of the reconstructed
    // spectrum before adding its separately quantised gain. Unequal block
    // lengths make the otherwise independent PRBA codebooks interact here.
    for (auto& column : basis) {
        float mean = 0.0f;
        for (int l = 0; l < L; ++l) mean += column[l];
        mean /= count;
        for (int l = 0; l < L; ++l) column[l] -= mean;
    }
    float residualMean = 0.0f, targetMean = 0.0f;
    for (int l = 0; l < L; ++l) {
        residualMean += residual[l];
        targetMean += target[l];
    }
    residualMean /= count;
    targetMean /= count;

    float coefficients[kCoefficients] = {};
    for (int k = 0; k < 3; ++k) coefficients[k] = tables.prba24[b[3]][k];
    for (int k = 0; k < 4; ++k) coefficients[k + 3] = tables.prba58[b[4]][k];
    for (int block = 0; block < 4; ++block)
        for (int k = 0; k < 4; ++k) coefficients[7 + block * 4 + k] = tables.hoc[block][b[5 + block]][k];

    float error[kHarmonics], weight[kHarmonics];
    for (int l = 0; l < L; ++l) {
        error[l] = residualMean - residual[l];
        for (int k = 0; k < kCoefficients; ++k) error[l] += coefficients[k] * basis[k][l];
        // Square-root amplitude weighting protects stronger speech harmonics
        // without letting a single formant dominate. The bounds retain quiet
        // detail and bound the numerical range. See docs/encoder-quality.md.
        weight[l] = std::exp2(0.5f * (target[l] - targetMean));
        if (weight[l] < 0.1f) weight[l] = 0.1f;
        if (weight[l] > 10.0f) weight[l] = 10.0f;
    }

    // Two coordinate-descent passes from the ordinary quantiser's solution.
    // Every accepted replacement lowers this frame's weighted shape error;
    // this is a local search, not an exhaustive optimum or a perceptual proof.
    for (int pass = 0; pass < 2; ++pass) {
        for (int group = 0; group < 6; ++group) {
            const int first = group == 0 ? 0 : group == 1 ? 3 : 7 + (group - 2) * 4;
            const int dimensions = group == 0 ? 3 : 4;
            const int rows = group == 0 ? 512 : group == 1 ? 128 : tables.hocRows[group - 2];
            const int step = group < 2 ? 1 : tables.hocStep[group - 2];
            float matrix[4][4] = {}, linear[4] = {};
            for (int a = 0; a < dimensions; ++a) {
                for (int l = 0; l < L; ++l) {
                    linear[a] += weight[l] * error[l] * basis[first + a][l];
                    for (int other = 0; other < dimensions; ++other)
                        matrix[a][other] += weight[l] * basis[first + a][l] * basis[first + other][l];
                }
            }
            float bestCost = 0.0f;
            int best = b[3 + group];
            for (int row = 0; row < rows; row += step) {
                float difference[4] = {};
                for (int a = 0; a < dimensions; ++a)
                    difference[a] = codeword(tables, group, row, a) - coefficients[first + a];
                float cost = 0.0f;
                for (int a = 0; a < dimensions; ++a) {
                    cost += 2.0f * difference[a] * linear[a];
                    for (int other = 0; other < dimensions; ++other)
                        cost += difference[a] * difference[other] * matrix[a][other];
                }
                if (cost < bestCost) { bestCost = cost; best = row; }
            }
            b[3 + group] = best;
            for (int a = 0; a < dimensions; ++a) {
                const float next = codeword(tables, group, best, a);
                for (int l = 0; l < L; ++l) error[l] += (next - coefficients[first + a]) * basis[first + a][l];
                coefficients[first + a] = next;
            }
        }
    }
}
} // namespace mbe
