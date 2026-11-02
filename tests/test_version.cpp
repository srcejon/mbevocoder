// SPDX-License-Identifier: GPL-3.0-or-later
#include <mbevocoder/mbevocoder.h>
#include <mbelib-neo/mbelib.h>   // the compat headers must also compile, together
#include <mbelib-neo/version.h>
#include <cstdio>
#include <cstring>

int main()
{
    const char* v = mbe_versionString();
    if (!v || std::strcmp(v, MBEVOCODER_VERSION) != 0 || std::strcmp(MBELIB_VERSION, MBEVOCODER_VERSION) != 0) {
        std::fprintf(stderr, "version mismatch: %s vs %s\n", v ? v : "(null)", MBEVOCODER_VERSION);
        return 1;
    }
    std::printf("mbevocoder %s\n", v);
    return 0;
}
