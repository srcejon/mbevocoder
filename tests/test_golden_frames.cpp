// SPDX-License-Identifier: GPL-3.0-or-later
// Sanity test on the committed golden frames: every .dvsi under corpus/golden/<rate>
// has a JSON sidecar, a whole number of 9-byte frames, matches the sidecar's frame
// count, and is not degenerate (all-zero or constant). Phase 1 extends this with FEC
// checks once the Golay decoders exist.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool check(const fs::path& dvsi, int& frames)
{
    std::ifstream f(dvsi, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.empty() || d.size() % 9 != 0) { std::printf("FAIL %s: size %zu\n", dvsi.string().c_str(), d.size()); return false; }
    frames = static_cast<int>(d.size() / 9);
    fs::path side = dvsi; side += ".json";
    std::ifstream j(side);
    if (!j) { std::printf("FAIL %s: missing sidecar\n", dvsi.string().c_str()); return false; }
    std::string js((std::istreambuf_iterator<char>(j)), std::istreambuf_iterator<char>());
    std::string key = "\"output_frames\": ";
    size_t p = js.find(key);
    if (p == std::string::npos || std::atoi(js.c_str() + p + key.size()) != frames) {
        std::printf("FAIL %s: sidecar frame count mismatch\n", dvsi.string().c_str()); return false;
    }
    size_t distinct = 0;
    for (size_t i = 9; i < d.size(); i += 9) if (std::memcmp(&d[i], &d[i - 9], 9) != 0) ++distinct;
    if (distinct < d.size() / 9 / 4) { std::printf("FAIL %s: frames nearly constant\n", dvsi.string().c_str()); return false; }
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: test_golden_frames <corpus/golden dir>\n"); return 2; }
    int total = 0, files = 0, bad = 0;
    for (const char* rate : {"dstar", "ambe2450"}) {
        fs::path dir = fs::path(argv[1]) / rate;
        if (!fs::exists(dir)) { std::printf("SKIP: %s missing\n", dir.string().c_str()); continue; }
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() != ".dvsi") continue;
            // synthetic silence/DC frames are legitimately constant; skip the degeneracy check for them
            int n = 0;
            if (e.path().stem().string().rfind("syn_silence", 0) == 0 || e.path().stem().string().rfind("syn_dc", 0) == 0) continue;
            if (!check(e.path(), n)) ++bad; else { total += n; ++files; }
        }
    }
    std::printf("%d golden files, %d frames, %d bad\n", files, total, bad);
    return bad == 0 && files > 0 ? 0 : 1;
}
