// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal RIFF/WAVE reader and writer for 16-bit PCM mono.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace wav {

struct Audio {
    unsigned sampleRate = 8000;
    std::vector<int16_t> samples;   // mono
};

inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline Audio read(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(f);
    if (buf.size() < 12 || std::string(buf.begin(), buf.begin() + 4) != "RIFF" || std::string(buf.begin() + 8, buf.begin() + 12) != "WAVE")
        throw std::runtime_error(path + ": not a WAVE file");

    Audio a;
    unsigned channels = 0, bits = 0, format = 0;
    size_t pos = 12;
    bool gotData = false;
    while (pos + 8 <= buf.size()) {
        std::string id(buf.begin() + static_cast<long>(pos), buf.begin() + static_cast<long>(pos) + 4);
        uint32_t len = rd32(&buf[pos + 4]);
        size_t body = pos + 8;
        if (id == "fmt ") {
            if (len < 16) throw std::runtime_error(path + ": bad fmt chunk");
            format = rd16(&buf[body]);
            channels = rd16(&buf[body + 2]);
            a.sampleRate = rd32(&buf[body + 4]);
            bits = rd16(&buf[body + 14]);
            if (format == 0xFFFE && len >= 40) format = rd16(&buf[body + 24]);   // WAVE_FORMAT_EXTENSIBLE
        } else if (id == "data") {
            if (format != 1 || bits != 16) throw std::runtime_error(path + ": only 16-bit PCM is supported");
            if (channels < 1) throw std::runtime_error(path + ": bad channel count");
            size_t avail = std::min<size_t>(len, buf.size() - body);
            size_t frames = avail / (2 * channels);
            a.samples.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                // take channel 0 (corpus files are mono anyway)
                a.samples[i] = static_cast<int16_t>(rd16(&buf[body + i * 2 * channels]));
            }
            gotData = true;
            break;
        }
        pos = body + len + (len & 1);
    }
    if (!gotData) throw std::runtime_error(path + ": no data chunk");
    return a;
}

inline void write(const std::string& path, const Audio& a)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot create " + path);
    auto w32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto w16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    uint32_t dataLen = static_cast<uint32_t>(a.samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataLen); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(1); w32(a.sampleRate); w32(a.sampleRate * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, f); w32(dataLen);
    for (int16_t s : a.samples) w16(static_cast<uint16_t>(s));
    std::fclose(f);
}

} // namespace wav
