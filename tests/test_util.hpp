// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny assertion helpers shared by the test executables (no framework dependency).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace test {

inline int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++test::failures;                                                             \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        auto _a = (a); auto _b = (b);                                                                      \
        if (!(_a == _b)) {                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__,  \
                         #a, static_cast<long long>(_a), #b, static_cast<long long>(_b));                  \
            ++test::failures;                                                                              \
        }                                                                                                  \
    } while (0)

inline int finish(const char* name)
{
    if (failures) { std::fprintf(stderr, "%s: %d failure(s)\n", name, failures); return 1; }
    std::printf("%s: ok\n", name);
    return 0;
}

// Deterministic LCG for reproducible pseudo-random test data.
struct Rng {
    uint32_t x;
    explicit Rng(uint32_t seed) : x(seed * 2654435761u + 12345u) {}
    uint32_t next() { x = x * 1103515245u + 12345u; return (x >> 8) & 0x7fffffu; }
    uint32_t below(uint32_t n) { return next() % n; }
    char bit() { return static_cast<char>(next() & 1u); }
};

// Flip `count` distinct positions of a bit array; identical generator in tools/neo-decode.
inline void flipBits(char* bits, unsigned nbits, unsigned count, uint32_t frameIndex)
{
    Rng r(frameIndex + 1);
    std::vector<char> done(nbits, 0);
    for (unsigned k = 0; k < count;) {
        uint32_t p = r.below(nbits);
        if (done[p]) continue;
        done[p] = 1;
        bits[p] ^= 1;
        ++k;
    }
}

inline std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace test
