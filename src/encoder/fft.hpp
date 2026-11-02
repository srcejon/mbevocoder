// SPDX-License-Identifier: GPL-3.0-or-later
// In-place radix-2 complex FFT for the encoder's front end (tone detection, noise
// suppression): small, power-of-two sizes, no tables.
#pragma once

#include <cmath>

namespace mbe {

inline void fftRadix2(float* re, float* im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * 3.14159265358979f / static_cast<float>(len);
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

// Inverse transform (unscaled: divide by n afterwards).
inline void ifftRadix2(float* re, float* im, int n)
{
    for (int i = 0; i < n; ++i) im[i] = -im[i];
    fftRadix2(re, im, n);
    for (int i = 0; i < n; ++i) im[i] = -im[i];
}

} // namespace mbe
