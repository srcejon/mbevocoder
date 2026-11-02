// SPDX-License-Identifier: GPL-3.0-or-later
// Full decode path (frame -> PCM) compared with mbelib-neo dumps produced by
// `neo-decode --dump`: clean frames and frames with 0..5 injected bit errors. Each file
// is decoded on its own thread so thread-local RNG state starts fresh, as it does in
// the one-process-per-file dump.
//
//   test_decoder_parity <corpus/golden dir> <neo dump dir> [max abs diff, default 1.0]
#include <mbevocoder/mbevocoder.h>

#include "test_util.hpp"
#include "parity_synth.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

struct Rate {
    const char* name;
    mbe_layout layout;
    bool lsbFirst;
    int (*process)(float*, mbe_process_result*, const char[4][24], char[49], mbe_parms*, mbe_parms*, mbe_parms*);
};

static const Rate kRates[2] = {
    {"dstar", MBE_LAYOUT_DSTAR, true, mbe_processAmbe3600x2400Framef},
    {"ambe2450", MBE_LAYOUT_DMR, false, mbe_processAmbe3600x2450Framef},
};

struct FileStats {
    long frames = 0, flagMismatch = 0, statusMismatch = 0;
    double maxDiffClean = 0, maxDiffNoisy = 0;
    long framesOverTol = 0;
    std::string worst;
};

// mbelib-neo clips its float output at 95 % of full scale on the 1/7 scale; mbevocoder does
// not (MBEVOCODER_FLOAT_CLIP off), so its samples are clipped the same way before comparing.
// AMBE+2 voicing rows mbevocoder renders from the AMBE-3000R's measurements rather than mbelib's
// table (MBEVOCODER_CHIP_MEASURED, mixed excitation): their frames and a window after them are not
// compared with mbelib-neo's waveform; the rows that are plain voiced/unvoiced on the AMBE-3000R
// too (0, 2, 3, 4, 9, 10, 14, 16, about 70 % of speech frames) stay bit-exact.
static bool chipVuvRow(int b1)
{
    // rows whose measured mixed excitation (src/decoder/ambe3600x2450_vuvmix.h) is not
    // mbelib's plain voiced/unvoiced rendering: partial bands, or level offsets
    static const bool differs[32] = {false, true, false, false, false, true, true, true, true, false, false, true, true, true, false, true,
                                     false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};
    return b1 >= 0 && b1 < 32 && differs[b1];
}

// Exemption window: a measured row's frame and the five after it (synthesis overlap, phase
// regeneration and the noise overlap), restarted by any repeated/erased frame in between
// (the row's parameters live on).
struct VuvExempt {
    int left = 0;
    bool frame(int row, int status)
    {
        if (chipVuvRow(row)) left = 6;
        else if (status != 0 && left > 0) left = 6;
        if (left > 0) { --left; return true; }
        return false;
    }
};

// A C0 word the extended Golay code cannot correct: mbevocoder (MBEVOCODER_CHIP_MEASURED) reports
// c0_errors 4 and repeats the frame as the AMBE-3000R does, mbelib-neo decodes it; the frame
// and the state it leaves behind (repeat count, prediction) are not compared for a few frames.
struct C0Exempt {
    int left = 0;
    bool frame(int c0)
    {
        if (c0 >= 4) left = 6;
        if (left > 0) { --left; return true; }
        return false;
    }
};

static float neoClip(float v)
{
    const float lim = 32767.0f * 0.95f / 7.0f;
    return v > lim ? lim : (v < -lim ? -lim : v);
}

static std::vector<float> readF32(const fs::path& p)
{
    auto raw = test::readFile(p.string());
    std::vector<float> v(raw.size() / 4);
    std::memcpy(v.data(), raw.data(), v.size() * 4);
    return v;
}

static void decodeFile(const Rate& rate, const fs::path& dvsi, const fs::path& neo, double tol, FileStats& st)
{
    auto data = test::readFile(dvsi.string());
    std::vector<std::string> lines;
    {
        std::ifstream nf(neo.string());
        std::string line;
        while (std::getline(nf, line)) lines.push_back(line);
    }
    auto noisyRef = readF32(neo.string() + ".noisy.f32");
    auto cleanRef = readF32(neo.string() + ".clean.f32");
    size_t n = data.size() / 9;
    if (lines.size() != n || noisyRef.size() != n * 160 || cleanRef.size() != n * 160) {
        std::fprintf(stderr, "%s: dump size mismatch\n", neo.string().c_str());
        st.statusMismatch = 1;
        return;
    }
    mbe_parms cur, prev, prevEnh, ccur, cprev, cprevEnh;
    mbe_initMbeParms(&cur, &prev, &prevEnh);
    mbe_initMbeParms(&ccur, &cprev, &cprevEnh);
    VuvExempt exNoisy, exClean;
    C0Exempt exC0;
    for (size_t i = 0; i < n; ++i) {
        char bits[72], noisy[72], fr[4][24], cfr[4][24], d[49];
        mbe_unpackBits(&data[i * 9], 72, bits, rate.lsbFirst ? 0 : 1);
        std::memcpy(noisy, bits, 72);
        std::istringstream ss(lines[i]);
        unsigned nflip = 0; int c0 = 0, total = 0; std::string bitsRef; unsigned flagsRef = 0; int statusRef = 0;
        ss >> nflip >> c0 >> total >> bitsRef >> flagsRef >> statusRef;
        test::flipBits(noisy, 72, nflip, static_cast<uint32_t>(i));
        mbe_ambeFrameFromBits(rate.layout, noisy, fr);
        mbe_ambeFrameFromBits(rate.layout, bits, cfr);
        float pcm[160];
        mbe_process_result r;
        int status = rate.process(pcm, &r, fr, d, &cur, &prev, &prevEnh);
        const bool exemptErr = exC0.frame(r.c0_errors);
        const bool is2450 = !rate.lsbFirst;
        // the dumps are mbelib-neo 2.1's, which had no flags above MUTE (2.4's SILENCE and PROVOICE)
        unsigned neo21Flags = 2u * MBE_PROCESS_FLAG_MUTE - 1u;
#if MBEVOCODER_CHIP_MEASURED
        {
            // the counts are full-weight (TIA-102.BABA-1 5.5, mbelib-neo 2.6): the status is the
            // distance of the received C0 and C1 (D-STAR: and C1's parity cell) from the corrected
            // frame re-encoded, and the repeat and mute decisions are the AMBE-3000R's (error-rate
            // mutes, a repeat only for an uncorrectable C0), so REPEAT and MUTE are not compared with
            // mbelib-neo 2.1's
            char efr[4][24];
            if (is2450) mbe_encodeAmbe3600x2450Frame(d, efr); else mbe_encodeAmbe3600x2400Frame(d, efr);
            int d0 = 0, d1 = 0;
            for (int j = 0; j < 24; ++j) d0 += fr[0][j] != efr[0][j];
            for (int j = 0; j < 23; ++j) d1 += fr[1][j] != efr[1][j];
            if (!is2450) d1 += fr[2][10] != efr[2][10];
            statusRef = (d0 < 4 ? d0 : 3) + d1;
            neo21Flags &= ~(MBE_PROCESS_FLAG_REPEAT | MBE_PROCESS_FLAG_MUTE);
            flagsRef &= neo21Flags;
        }
#endif
        if (!exemptErr && status != statusRef) ++st.statusMismatch;
        if (!exemptErr && (r.flags & neo21Flags) != flagsRef) ++st.flagMismatch;
        int b1 = (d[4] << 4) | (d[5] << 3) | (d[6] << 2) | (d[7] << 1) | d[35];
        const bool exemptNoisy = exemptErr || (is2450 && exNoisy.frame(b1, status));
        double md = 0;
        if (!exemptNoisy)
            for (int k = 0; k < 160; ++k) md = std::max(md, static_cast<double>(std::fabs(neoClip(pcm[k]) - noisyRef[i * 160 + k])));
        if (md > st.maxDiffNoisy) { st.maxDiffNoisy = md; st.worst = dvsi.filename().string() + " frame " + std::to_string(i) + " (noisy)"; }
        if (md > tol) ++st.framesOverTol;
        status = rate.process(pcm, &r, cfr, d, &ccur, &cprev, &cprevEnh);
        b1 = (d[4] << 4) | (d[5] << 3) | (d[6] << 2) | (d[7] << 1) | d[35];
        const bool exemptClean = is2450 && exClean.frame(b1, status);
        md = 0;
        if (!exemptClean)
            for (int k = 0; k < 160; ++k) md = std::max(md, static_cast<double>(std::fabs(neoClip(pcm[k]) - cleanRef[i * 160 + k])));
        if (md > st.maxDiffClean) { st.maxDiffClean = md; if (md > st.maxDiffNoisy) st.worst = dvsi.filename().string() + " frame " + std::to_string(i) + " (clean)"; }
        if (md > tol) ++st.framesOverTol;
        ++st.frames;
    }
}

// Synthetic cases: run every case on its own thread (fresh thread-local state, as in the
// neo-decode process that produced the reference) and compare with <neoDir>/synth.<case>.*
static void syntheticCases(const fs::path& neoDir, const fs::path& tmpDir, double tol, bool dstarPcm, bool voicedPcm, bool imbeMeta)
{
    fs::create_directories(tmpDir);
    for (const parity::Case& c : parity::cases()) {
        fs::path ref = neoDir / ("synth." + std::string(c.name) + ".f32");
        if (!fs::exists(ref)) { std::printf("SKIP synth %s (no reference)\n", c.name); continue; }
        std::string prefix = (tmpDir / "ours").string();
        std::thread t([&] { parity::runCase(c, prefix); });
        t.join();
        auto a = readF32(prefix + "." + c.name + ".f32");
        auto b = readF32(ref);
        std::ifstream ta(prefix + "." + c.name + ".txt"), tb((neoDir / ("synth." + std::string(c.name) + ".txt")).string());
        std::ifstream trows(prefix + "." + c.name + ".vuvrow");
        std::ifstream tc0(prefix + "." + c.name + ".c0err");
        VuvExempt ex;
        C0Exempt exC0;
        long over = 0, metaMismatch = 0, toneFrames = 0;
        double maxDiff = 0;
        CHECK_EQ(a.size(), b.size());
        for (size_t i = 0; i + 160 <= std::min(a.size(), b.size()); i += 160) {
            std::string la, lb;
            std::getline(ta, la); std::getline(tb, lb);
            int c0err = 0;
            tc0 >> c0err;
            if (exC0.frame(c0err)) continue;   // a detected-uncorrectable C0 word and its aftermath
            // the reference dumps may carry Windows line endings (CR, 13) on a platform that does not translate them
            while (!la.empty() && la.back() == 13) la.pop_back();
            while (!lb.empty() && lb.back() == 13) lb.pop_back();
            {   // "status flags ...": mbelib-neo 2.1 had no flags above MUTE (2.4's SILENCE and PROVOICE)
                int st0 = 0, pos = 0;
                unsigned fl = 0;
                if (std::sscanf(la.c_str(), "%d %u%n", &st0, &fl, &pos) == 2)
                    la = std::to_string(st0) + " " + std::to_string(fl & (2u * MBE_PROCESS_FLAG_MUTE - 1u)) + la.substr(static_cast<size_t>(pos));
            }
            // MBEVOCODER_CHIP_MEASURED: AMBE+2 erasures and unusable tone frames are repeats, as the
            // AMBE-3000R plays them, where mbelib-neo 2.1 played comfort noise (docs/decoder-quality.md)
            unsigned flOurs = 0, flRef = 0;
            int stTmp = 0;
            std::sscanf(la.c_str(), "%d %u", &stTmp, &flOurs);
            std::sscanf(lb.c_str(), "%d %u", &stTmp, &flRef);
            const bool erasure = !imbeMeta && std::string(c.name).rfind("ambe2450", 0) == 0
                                 && ((flOurs | flRef) & MBE_PROCESS_FLAG_ERASURE) != 0u;
            if (la != lb && !erasure) ++metaMismatch;
            // Tone frames: mbevocoder renders the AMBE-3000R's tone amplitude law
            // (docs/tone-frames.md), mbelib-neo a linear one, so their waveforms differ by
            // design; the status/flag line above is still compared.
            int status = 0, flags = 0;
            std::sscanf(la.c_str(), "%d %d", &status, &flags);
            int row = -1;
            trows >> row;
            const bool exemptRow = ex.frame(row, status);
            if (flags & MBE_PROCESS_FLAG_TONE) { ++toneFrames; continue; }
            if (exemptRow) continue;
            double md = 0;
            const bool shortPath = std::string(c.name).find("short") != std::string::npos;   // 16-bit output: already clipped by mbe_floattoshort
            for (int k = 0; k < 160; ++k) md = std::max(md, static_cast<double>(std::fabs((shortPath ? a[i + k] : neoClip(a[i + k])) - b[i + k])));
            maxDiff = std::max(maxDiff, md);
            if (md > tol) ++over;
        }
        bool isDstar = std::string(c.name).find("2400") != std::string::npos;
        // cases whose status lines differ from mbelib-neo's by design with MBEVOCODER_CHIP_MEASURED:
        // IMBE 7200 (Golay parity errors counted), D-STAR (tone frames under errors played or
        // repeated as the AMBE-3000R does), the soft-decision frames (C0 trusted when the bits the
        // soft search overrules are weak), AMBE+2 frames (full-weight error counts and the
        // error-rate mute)
        const std::string name(c.name);
        const bool isImbe7200 = name.rfind("imbe7200", 0) == 0 || name == "ambe2400_data" || name == "ambe2400_softframe"
                                || name == "ambe2450_softframe" || name == "ambe2450_short";
        std::printf("synth %-20s %5u frames, max |diff| %.4g, over tol %ld%s, status/flag/error mismatches %ld%s, tone frames %ld\n", c.name, c.frames, maxDiff, over,
                    (isDstar && !dstarPcm) ? " (waveform not gated: measured D-STAR f0)" : "", metaMismatch,
                    (isImbe7200 && !imbeMeta) ? " (not gated: measured error handling)" : "", toneFrames);
        if (voicedPcm && (dstarPcm || !isDstar)) CHECK_EQ(over, 0);
        if (imbeMeta || !isImbe7200) CHECK_EQ(metaMismatch, 0);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: test_decoder_parity <golden dir> <neo dump dir> [tol] [tmp dir]\n"); return 2; }
    fs::path golden = argv[1], neoDir = argv[2];
    double tol = argc > 3 ? std::atof(argv[3]) : 1.0;
    fs::path tmpDir = argc > 4 ? fs::path(argv[4]) : fs::temp_directory_path() / "mbevocoder_parity";
    // With the measured D-STAR fundamental mapping (MBEVOCODER_CHIP_MEASURED) the 3600x2400
    // waveform deliberately differs from mbelib-neo; flags and statuses must still agree.
    bool dstarPcm = !(argc > 5 && std::string(argv[5]) == "--no-dstar-pcm");
    // With MBEVOCODER_CHIP_MEASURED the voiced synthesis ramps amplitudes geometrically for every
    // rate, so no waveform matches mbelib-neo's; parameters, flags and statuses still must.
    // Configure with MBEVOCODER_CHIP_MEASURED=OFF (and the other AMBE-3000R options off) for the
    // bit-exact waveform check.
    bool voicedPcm = !(argc > 6 && std::string(argv[6]) == "--no-voiced-pcm");
    if (!voicedPcm) std::printf("waveforms not gated: MBEVOCODER_CHIP_MEASURED in use (parameters, flags and statuses are)\n");
    // With MBEVOCODER_CHIP_MEASURED the error handling follows the standard and the AMBE-3000R where
    // they differ from mbelib-neo (the IMBE 7200 Golay parity count, D-STAR tone frames under
    // errors, soft-decision C0), so those cases' statuses differ on damaged frames;
    // tests/test_ecc.cpp and tests/test_robustness.cpp check them.
    bool imbeMeta = !(argc > 7 && std::string(argv[7]) == "--no-imbe-meta");
    syntheticCases(neoDir, tmpDir, tol, dstarPcm && voicedPcm, voicedPcm, imbeMeta);
    for (const Rate& rate : kRates) {
        fs::path d = golden / rate.name;
        if (!fs::exists(d) || !fs::exists(neoDir / rate.name)) { std::printf("SKIP %s\n", rate.name); continue; }
        FileStats agg;
        long files = 0;
        for (const auto& e : fs::directory_iterator(d)) {
            if (e.path().extension() != ".dvsi") continue;
            fs::path neo = neoDir / rate.name / (e.path().stem().string() + ".neo");
            if (!fs::exists(neo)) continue;
            FileStats st;
            std::thread t([&] { decodeFile(rate, e.path(), neo, tol, st); });
            t.join();
            ++files;
            agg.frames += st.frames;
            agg.flagMismatch += st.flagMismatch;
            agg.statusMismatch += st.statusMismatch;
            agg.framesOverTol += st.framesOverTol;
            if (st.maxDiffNoisy > agg.maxDiffNoisy) { agg.maxDiffNoisy = st.maxDiffNoisy; agg.worst = st.worst; }
            if (st.maxDiffClean > agg.maxDiffClean) agg.maxDiffClean = st.maxDiffClean;
        }
        std::printf("%s: %ld files, %ld frames, max |diff| clean %.4g noisy %.4g, frames over tol %ld, flag mismatches %ld, status mismatches %ld\n",
                    rate.name, files, agg.frames, agg.maxDiffClean, agg.maxDiffNoisy, agg.framesOverTol, agg.flagMismatch, agg.statusMismatch);
        if (!agg.worst.empty()) std::printf("  worst: %s\n", agg.worst.c_str());
        bool gatePcm = voicedPcm && (dstarPcm || std::string(rate.name) != "dstar");
        if (!gatePcm) std::printf("  (waveform not gated for dstar: measured D-STAR f0 mapping in use)\n");
        CHECK(files > 0);
        if (gatePcm) CHECK_EQ(agg.framesOverTol, 0);
        CHECK_EQ(agg.flagMismatch, 0);
        CHECK_EQ(agg.statusMismatch, 0);
    }
    return test::finish("test_decoder_parity");
}
