// SPDX-License-Identifier: GPL-3.0-or-later
// AMBE-3000R-captured frames through the bit layer:
//  1. every golden frame decodes with zero FEC corrections under the documented layout
//  2. parameter bits -> mbe_encode*Frame reproduces the AMBE-3000R frame exactly (validates the
//     encoder-side assembly, including the D-STAR C1 parity position)
//  3. the raw 49-bit capture (rate 34) equals the 3600x2450 capture's parameter bits under
//     the DVSI raw order
//  4. optional: parity with an mbelib-neo dump produced by `neo-decode --dump` under
//     deterministic injected errors
//
//   test_golden_bits <corpus/golden dir> <tests/data dir> [neo dump dir]
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"

#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

struct Rate {
    const char* name;
    mbe_layout layout;
    bool lsbFirst;
    int (*decode)(const char[4][24], char[49], mbe_process_result*);
    int (*encode)(const char[49], char[4][24]);
};

static const Rate kRates[2] = {
    {"dstar", MBE_LAYOUT_DSTAR, true, mbe_decodeAmbe3600x2400Frame, mbe_encodeAmbe3600x2400Frame},
    {"ambe2450", MBE_LAYOUT_DMR, false, mbe_decodeAmbe3600x2450Frame, mbe_encodeAmbe3600x2450Frame},
};

static void goldenFrames(const fs::path& dir, const fs::path& neoDir)
{
    for (const Rate& rate : kRates) {
        fs::path d = dir / rate.name;
        if (!fs::exists(d)) { std::printf("SKIP %s: %s missing\n", rate.name, d.string().c_str()); continue; }
        long frames = 0, files = 0, errFrames = 0, mismatches = 0, neoFrames = 0, neoMismatch = 0, neoUncorrectable = 0;
        for (const auto& e : fs::directory_iterator(d)) {
            if (e.path().extension() != ".dvsi") continue;
            auto data = test::readFile(e.path().string());
            ++files;
            // optional mbelib-neo dump for this file
            std::vector<std::string> neoLines;
            if (!neoDir.empty()) {
                std::ifstream nf((neoDir / rate.name / (e.path().stem().string() + ".neo")).string());
                std::string line;
                while (std::getline(nf, line)) neoLines.push_back(line);
            }
            for (size_t i = 0; i + 9 <= data.size(); i += 9, ++frames) {
                char bits[72], fr[4][24], dbits[49];
                mbe_unpackBits(&data[i], 72, bits, rate.lsbFirst ? 0 : 1);
                CHECK_EQ(mbe_ambeFrameFromBits(rate.layout, bits, fr), 0);
                mbe_process_result r;
                int errs = rate.decode(fr, dbits, &r);
                if (errs != 0) ++errFrames;
                char fr2[4][24];
                CHECK_EQ(rate.encode(dbits, fr2), 0);
                if (std::memcmp(fr, fr2, sizeof fr) != 0) {
                    if (mismatches < 3) {
                        std::fprintf(stderr, "%s %s frame %zu: reassembled frame differs at cells:", rate.name,
                                     e.path().filename().string().c_str(), i / 9);
                        for (int row = 0; row < 4; ++row)
                            for (int col = 0; col < 24; ++col)
                                if (fr[row][col] != fr2[row][col]) std::fprintf(stderr, " [%d][%d]", row, col);
                        std::fprintf(stderr, "\n");
                    }
                    ++mismatches;
                }
                // mbelib-neo parity under injected errors
                size_t fi = i / 9;
                if (fi < neoLines.size()) {
                    std::istringstream ss(neoLines[fi]);
                    unsigned nflip = 0; int neoC0 = 0, neoTotal = 0; std::string neoBits;
                    ss >> nflip >> neoC0 >> neoTotal >> neoBits;
                    char noisy[72];
                    std::memcpy(noisy, bits, 72);
                    test::flipBits(noisy, 72, nflip, static_cast<uint32_t>(fi));
                    char nfr[4][24], nd[49];
                    mbe_ambeFrameFromBits(rate.layout, noisy, nfr);
                    int total = rate.decode(nfr, nd, &r);
                    std::string ours;
                    for (char c : nd) ours += static_cast<char>('0' + c);
                    ++neoFrames;
                    // a C0 word the extended Golay code cannot correct: mbevocoder reports 4 and repeats
                    // the frame (MBEVOCODER_CHIP_MEASURED), mbelib-neo counts the data bits it changed
                    int expC0 = neoC0, expTotal = neoTotal;
#if MBEVOCODER_CHIP_MEASURED
                    {
                        // the counts are full-weight (TIA-102.BABA-1 5.5, mbelib-neo 2.6): the received
                        // C0 and C1 cells that differ from the corrected frame re-encoded (C1 is
                        // scrambled with the same sequence on both sides, so the distance is the
                        // same); D-STAR's C1 parity bit, fr[2][10], counts too
                        char efr[4][24];
                        CHECK_EQ(rate.encode(nd, efr), 0);
                        int d0 = 0, d1 = 0;
                        for (int j = 0; j < 24; ++j) d0 += nfr[0][j] != efr[0][j];
                        for (int j = 0; j < 23; ++j) d1 += nfr[1][j] != efr[1][j];
                        if (rate.lsbFirst) d1 += nfr[2][10] != efr[2][10];
                        expC0 = d0 < 4 ? d0 : 3;
                        expTotal = expC0 + d1;
                    }
#endif
                    if (r.c0_errors == 4) { ++neoUncorrectable; }
                    else if (total != expTotal || r.c0_errors != expC0 || ours != neoBits) {
                        if (neoMismatch < 3)
                            std::fprintf(stderr, "%s %s frame %zu: expected (c0 %d total %d) vs ours (c0 %d total %d), bits %s\n",
                                         rate.name, e.path().filename().string().c_str(), fi, expC0, expTotal,
                                         r.c0_errors, total, ours == neoBits ? "equal" : "DIFFER");
                        ++neoMismatch;
                    }
                }
            }
        }
        std::printf("%s: %ld files, %ld frames, %ld with FEC corrections, %ld reassembly mismatches", rate.name, files,
                    frames, errFrames, mismatches);
        if (neoFrames) std::printf(", neo parity %ld/%ld (%ld uncorrectable C0 words repeated)", neoFrames - neoMismatch, neoFrames, neoUncorrectable);
        std::printf("\n");
        CHECK(files > 0);
        CHECK_EQ(errFrames, 0);
        CHECK_EQ(mismatches, 0);
        CHECK_EQ(neoMismatch, 0);
    }
}

static void rawOrder(const fs::path& dataDir)
{
    auto a = test::readFile((dataDir / "synth_ambe2450.dvsi").string());
    auto r = test::readFile((dataDir / "synth_raw2450.dvsi").string());
    size_t n = std::min(a.size() / 9, r.size() / 7);
    CHECK(n > 100);
    long match = 0;
    for (size_t k = 0; k < n; ++k) {
        char bits[72], fr[4][24], d[49], draw[49];
        mbe_unpackBits(&a[k * 9], 72, bits, 1);
        mbe_ambeFrameFromBits(MBE_LAYOUT_DMR, bits, fr);
        mbe_process_result res;
        CHECK_EQ(mbe_decodeAmbe3600x2450Frame(fr, d, &res), 0);
        CHECK_EQ(mbe_dvsiToAmbe2450Data(&r[k * 7], draw), 0);
        if (std::memcmp(d, draw, 49) == 0) ++match;
        uint8_t back[7];
        CHECK_EQ(mbe_ambe2450DataToDvsi(d, back), 0);
        CHECK(std::memcmp(back, &r[k * 7], 7) == 0);
    }
    std::printf("raw 2450 order: %ld/%zu frames match the 3600x2450 parameter bits\n", match, n);
    CHECK_EQ(match, static_cast<long>(n));
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: test_golden_bits <golden dir> <data dir> [neo dump dir]\n"); return 2; }
    goldenFrames(argv[1], argc > 3 ? fs::path(argv[3]) : fs::path());
    rawOrder(argv[2]);
    return test::finish("test_golden_bits");
}
