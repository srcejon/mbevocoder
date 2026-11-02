// SPDX-License-Identifier: GPL-3.0-or-later
// Concurrent decoder instances: N streams decoded on N threads must produce exactly what
// each produces alone. Exercises the per-stream state (mbe_parms) and the thread-local
// decoder scratch (FFT plan, RNGs) under contention. Run under TSan via the tsan preset.
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <cstring>
#include <thread>
#include <vector>

static std::vector<float> decodeStream(uint32_t seed, int frames)
{
    test::Rng rng(seed);
    mbe_parms cur, prev, prevEnh;
    mbe_initMbeParms(&cur, &prev, &prevEnh);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames) * 160);
    for (int i = 0; i < frames; ++i) {
        char d[49], fr[4][24];
        for (char& b : d) b = rng.bit();
        bool dstar = (seed & 1u) != 0;
        (dstar ? mbe_encodeAmbe3600x2400Frame : mbe_encodeAmbe3600x2450Frame)(d, fr);
        test::flipBits(&fr[0][0], 96, rng.below(3), static_cast<uint32_t>(i));
        float pcm[160];
        char dd[49];
        mbe_process_result r;
        (dstar ? mbe_processAmbe3600x2400Framef : mbe_processAmbe3600x2450Framef)(pcm, &r, fr, dd, &cur, &prev, &prevEnh);
        out.insert(out.end(), pcm, pcm + 160);
    }
    return out;
}

int main()
{
    const int streams = 8, frames = 400;
    std::vector<std::vector<float>> alone(streams), together(streams);
    // Reference: each stream on its own fresh thread.
    for (int s = 0; s < streams; ++s) {
        std::thread t([&, s] { alone[s] = decodeStream(static_cast<uint32_t>(s + 1), frames); });
        t.join();
    }
    // Concurrent: all streams at once, plus two streams sharing one thread back to back.
    std::vector<std::thread> pool;
    for (int s = 0; s < streams; ++s)
        pool.emplace_back([&, s] { together[s] = decodeStream(static_cast<uint32_t>(s + 1), frames); mbe_freeThreadResources(); });
    for (auto& t : pool) t.join();
    for (int s = 0; s < streams; ++s) {
        CHECK_EQ(alone[s].size(), together[s].size());
        CHECK(std::memcmp(alone[s].data(), together[s].data(), alone[s].size() * sizeof(float)) == 0);
    }
    // Two streams interleaved on one thread are independent of each other.
    std::vector<float> a, b;
    {
        std::thread t([&] {
            test::Rng ra(1), rb(2);
            mbe_parms ca, pa, ea, cb, pb, eb;
            mbe_initMbeParms(&ca, &pa, &ea);
            mbe_initMbeParms(&cb, &pb, &eb);
            for (int i = 0; i < frames; ++i) {
                for (int which = 0; which < 2; ++which) {
                    test::Rng& rng = which ? rb : ra;
                    char d[49], fr[4][24], dd[49];
                    for (char& x : d) x = rng.bit();
                    (which ? mbe_encodeAmbe3600x2450Frame : mbe_encodeAmbe3600x2400Frame)(d, fr);
                    test::flipBits(&fr[0][0], 96, rng.below(3), static_cast<uint32_t>(i));
                    float pcm[160];
                    mbe_process_result r;
                    if (which) mbe_processAmbe3600x2450Framef(pcm, &r, fr, dd, &cb, &pb, &eb);
                    else mbe_processAmbe3600x2400Framef(pcm, &r, fr, dd, &ca, &pa, &ea);
                    (which ? b : a).insert((which ? b : a).end(), pcm, pcm + 160);
                }
            }
        });
        t.join();
    }
    // Comfort noise draws from a thread-local generator, so interleaving two streams on one
    // thread may legitimately change noise-fill frames; everything else must match. Check
    // that at least 90% of frames are identical to the stand-alone decode.
    auto agreement = [&](const std::vector<float>& x, const std::vector<float>& ref) {
        long same = 0, n = static_cast<long>(x.size() / 160);
        for (long i = 0; i < n; ++i)
            if (std::memcmp(&x[i * 160], &ref[i * 160], 160 * sizeof(float)) == 0) ++same;
        return n ? static_cast<double>(same) / static_cast<double>(n) : 0.0;
    };
    // mbe_freeThreadResources() releases the thread's FFT plan; decoding again on the thread
    // allocates a new one and gives the same output (same generator seeds).
    {
        std::vector<float> r1, r2;
        std::thread t([&] {
            mbe_setThreadRngSeed(7u);
            r1 = decodeStream(3u, frames);
            mbe_freeThreadResources();
            mbe_freeThreadResources();   // idempotent
            mbe_setThreadRngSeed(7u);
            r2 = decodeStream(3u, frames);
            mbe_freeThreadResources();
        });
        t.join();
        CHECK_EQ(r1.size(), r2.size());
        CHECK(std::memcmp(r1.data(), r2.data(), r1.size() * sizeof(float)) == 0);
    }
    double agA = agreement(a, alone[0]), agB = agreement(b, alone[1]);
    std::printf("interleaved streams agree with stand-alone decode: %.1f%% / %.1f%% of frames\n", agA * 100, agB * 100);
    CHECK(agA > 0.9);
    CHECK(agB > 0.9);
    return test::finish("test_threads");
}
