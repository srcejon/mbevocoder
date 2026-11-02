// SPDX-License-Identifier: GPL-3.0-or-later
// Apply the existing library's P25 FEC/layout to raw 88-bit IMBE parameters.
#include <mbevocoder/mbevocoder.h>
#include <fstream>
#include <cstdint>
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::ofstream out(argv[2], std::ios::binary);
    if (!in || !out) return 2;
    uint8_t raw[11], packed[18];
    while (in.read(reinterpret_cast<char*>(raw), sizeof raw)) {
        char data[88], frame[8][23], bits[144];
        mbe_unpackBits(raw, 88, data, 1);
        if (mbe_encodeImbe7200x4400Frame(data, frame) < 0 ||
            mbe_imbeFrameToBits(MBE_LAYOUT_P25P1, frame, bits) < 0) return 1;
        mbe_packBits(bits, 144, packed, 1);
        out.write(reinterpret_cast<char*>(packed), sizeof packed);
    }
    return in.gcount() == 0 && out.good() ? 0 : 1;
}
