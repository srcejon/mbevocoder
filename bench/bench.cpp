// SPDX-License-Identifier: GPL-3.0-or-later
// mbevocoder benchmark: microseconds per 20 ms frame for every encoder, decoder and transcoder
// path, single threaded. Runs on synthetic speech by default; pass a directory of 8 kHz
// mono WAV files (e.g. testdata/corpus) to use real speech.
//
//   mbevocoder-bench [corpus-dir] [--seconds N] [--json out.json]
#include <mbevocoder/mbevocoder.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265f;

std::vector<short> synthSpeech(float seconds)
{
    const int n = static_cast<int>(seconds * 8000);
    std::vector<short> s(n);
    uint32_t rng = 12345;
    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / 8000.f;
        float f0 = 120.f + 60.f * std::sin(2 * kPi * 0.7f * t);
        float v = 0;
        for (int h = 1; h <= 25 && f0 * static_cast<float>(h) < 3900; ++h) v += std::sin(2 * kPi * f0 * static_cast<float>(h) * t) / static_cast<float>(h);
        rng = rng * 1664525u + 1013904223u;
        float noise = (static_cast<float>(rng >> 8) / 16777216.f - 0.5f);
        float env = 0.5f + 0.5f * std::sin(2 * kPi * 2.5f * t);
        float mix = (std::fmod(t, 1.0f) < 0.7f) ? v * 0.5f : noise * 0.6f;   // voiced then unvoiced stretches
        s[i] = static_cast<short>(9000.f * env * mix);
    }
    return s;
}

std::vector<short> readWav(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<short> out;
    if (data.size() < 44) return out;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        uint32_t len;
        std::memcpy(&len, &data[pos + 4], 4);
        if (std::memcmp(&data[pos], "data", 4) == 0) {
            size_t n = std::min<size_t>(len, data.size() - pos - 8) / 2;
            out.resize(n);
            std::memcpy(out.data(), &data[pos + 8], n * 2);
            return out;
        }
        pos += 8 + len + (len & 1);
    }
    return out;
}

struct Result { std::string name; double usPerFrame; long frames; };

template <class F>
Result timeIt(const char* name, long frames, F&& fn)
{
    // warm up, then time
    fn(std::min<long>(frames, 50));
    auto t0 = std::chrono::steady_clock::now();
    fn(frames);
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / static_cast<double>(frames);
    return {name, us, frames};
}

} // namespace

int main(int argc, char** argv)
{
    float seconds = 30.f;
    fs::path corpus, jsonOut;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::stof(argv[++i]);
        else if (a == "--json" && i + 1 < argc) jsonOut = argv[++i];
        else corpus = a;
    }
    std::vector<short> pcm;
    if (!corpus.empty() && fs::is_directory(corpus)) {
        for (const auto& e : fs::directory_iterator(corpus)) {
            if (e.path().extension() == ".wav") {
                auto s = readWav(e.path());
                pcm.insert(pcm.end(), s.begin(), s.end());
                if (pcm.size() >= static_cast<size_t>(seconds * 8000)) break;
            }
        }
    }
    if (pcm.empty()) pcm = synthSpeech(seconds);
    const long frames = static_cast<long>(pcm.size() / 160);
    std::printf("mbevocoder %s benchmark: %ld frames (%.1f s of %s), single thread\n", mbe_versionString(), frames, static_cast<double>(frames) * 0.02,
                corpus.empty() ? "synthetic speech" : corpus.string().c_str());

    // Encoders produce the frame streams the decoders and transcoders then consume.
    std::vector<std::vector<char>> ambeFrames(frames, std::vector<char>(96)), dstarFrames(frames, std::vector<char>(96)),
        imbeFrames(frames, std::vector<char>(184)), ambeData(frames, std::vector<char>(49)), imbeData(frames, std::vector<char>(88));
    std::vector<Result> results;

    auto encodeBench = [&](const char* name, mbe_rate rate, auto store) {
        return timeIt(name, frames, [&](long n) {
            mbe_encoder* e = mbe_encoderCreate(rate, nullptr);
            for (long i = 0; i < n; ++i) store(e, i);
            mbe_encoderDestroy(e);
        });
    };
    results.push_back(encodeBench("encode AMBE+2 3600x2450 (analysis + quantise + FEC)", MBE_RATE_AMBE3600x2450, [&](mbe_encoder* e, long i) {
        mbe_encoderEncodeAmbeFrame(e, &pcm[i * 160], reinterpret_cast<char (*)[24]>(ambeFrames[i].data()));
    }));
    results.push_back(encodeBench("encode AMBE+2 2450 parameters only", MBE_RATE_AMBE2450, [&](mbe_encoder* e, long i) {
        mbe_encoderEncodeParms(e, &pcm[i * 160], ambeData[i].data(), nullptr);
    }));
    results.push_back(encodeBench("encode D-STAR 3600x2400", MBE_RATE_AMBE3600x2400, [&](mbe_encoder* e, long i) {
        mbe_encoderEncodeAmbeFrame(e, &pcm[i * 160], reinterpret_cast<char (*)[24]>(dstarFrames[i].data()));
    }));
    results.push_back(encodeBench("encode IMBE 7200x4400", MBE_RATE_IMBE7200x4400, [&](mbe_encoder* e, long i) {
        mbe_encoderEncodeImbeFrame(e, &pcm[i * 160], reinterpret_cast<char (*)[23]>(imbeFrames[i].data()));
    }));
    results.push_back(encodeBench("encode IMBE 4400 parameters only", MBE_RATE_IMBE4400, [&](mbe_encoder* e, long i) {
        mbe_encoderEncodeParms(e, &pcm[i * 160], imbeData[i].data(), nullptr);
    }));

    auto decodeBench = [&](const char* name, auto process) {
        return timeIt(name, frames, [&](long n) {
            mbe_parms cur, prev, enh;
            mbe_initMbeParms(&cur, &prev, &enh);
            float out[160];
            for (long i = 0; i < n; ++i) process(out, i, cur, prev, enh);
        });
    };
    results.push_back(decodeBench("decode AMBE+2 3600x2450 (FEC + parameters + synthesis)", [&](float* out, long i, mbe_parms& c, mbe_parms& p, mbe_parms& e) {
        char d[49];
        mbe_process_result r;
        mbe_processAmbe3600x2450Framef(out, &r, reinterpret_cast<char (*)[24]>(ambeFrames[i].data()), d, &c, &p, &e);
    }));
    results.push_back(decodeBench("decode AMBE+2 2450 parameters + synthesis", [&](float* out, long i, mbe_parms& c, mbe_parms& p, mbe_parms& e) {
        mbe_process_result r;
        mbe_initProcessResult(&r);
        mbe_processAmbe2450Dataf(out, &r, ambeData[i].data(), &c, &p, &e);
    }));
    results.push_back(decodeBench("decode D-STAR 3600x2400", [&](float* out, long i, mbe_parms& c, mbe_parms& p, mbe_parms& e) {
        char d[49];
        mbe_process_result r;
        mbe_processAmbe3600x2400Framef(out, &r, reinterpret_cast<char (*)[24]>(dstarFrames[i].data()), d, &c, &p, &e);
    }));
    results.push_back(decodeBench("decode IMBE 7200x4400", [&](float* out, long i, mbe_parms& c, mbe_parms& p, mbe_parms& e) {
        char d[88];
        mbe_process_result r;
        mbe_processImbe7200x4400Framef(out, &r, reinterpret_cast<char (*)[23]>(imbeFrames[i].data()), d, &c, &p, &e);
    }));

    // Soft-decision frames: the hard frames with graded reliabilities and a weak wrong bit in
    // one frame in four, so the soft searches have something to decide.
    auto soften = [&](const std::vector<std::vector<char>>& hard) {
        std::vector<std::vector<mbe_soft_bit>> soft(hard.size());
        for (size_t i = 0; i < hard.size(); ++i) {
            soft[i].resize(hard[i].size());
            for (size_t k = 0; k < hard[i].size(); ++k)
                soft[i][k] = {static_cast<uint8_t>(hard[i][k] & 1), static_cast<uint8_t>(96 + (k * 37 + i * 11) % 160)};
            if (i % 4 == 0) { soft[i][5].bit ^= 1u; soft[i][5].reliability = 20; }
        }
        return soft;
    };
    const auto ambeSoft = soften(ambeFrames), imbeSoft = soften(imbeFrames);
    results.push_back(decodeBench("decode AMBE+2 3600x2450 soft bits", [&](float* out, long i, mbe_parms& c, mbe_parms& p, mbe_parms& e) {
        char d[49];
        mbe_process_result r;
        mbe_processAmbe3600x2450SoftFramef(out, &r, reinterpret_cast<const mbe_soft_bit (*)[24]>(ambeSoft[i].data()), d, &c, &p, &e);
    }));
    results.push_back(decodeBench("decode IMBE 7200x4400 soft bits", [&](float* out, long i, mbe_parms& c, mbe_parms& p, mbe_parms& e) {
        char d[88];
        mbe_process_result r;
        mbe_processImbe7200x4400SoftFramef(out, &r, reinterpret_cast<const mbe_soft_bit (*)[23]>(imbeSoft[i].data()), d, &c, &p, &e);
    }));

    auto transcodeBench = [&](const char* name, mbe_rate from, mbe_rate to, const std::vector<std::vector<char>>& src) {
        return timeIt(name, frames, [&](long n) {
            mbe_transcoder* t = mbe_transcoderCreate(from, to);
            char out[88];
            for (long i = 0; i < n; ++i) mbe_transcoderProcess(t, src[i].data(), out);
            mbe_transcoderDestroy(t);
        });
    };
    results.push_back(transcodeBench("transcode AMBE+2 -> D-STAR (parameters)", MBE_RATE_AMBE2450, MBE_RATE_AMBE3600x2400, ambeData));
    results.push_back(transcodeBench("transcode AMBE+2 -> IMBE (parameters)", MBE_RATE_AMBE2450, MBE_RATE_IMBE4400, ambeData));
    results.push_back(transcodeBench("transcode IMBE -> AMBE+2 (parameters)", MBE_RATE_IMBE4400, MBE_RATE_AMBE2450, imbeData));

    results.push_back(timeIt("tone frame AMBE+2 (build + FEC)", frames, [&](long n) {
        mbe_encoder* e = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, nullptr);
        char bits[72];
        for (long i = 0; i < n; ++i) mbe_encoderTone(e, 32 + static_cast<int>(i % 64), 100, MBE_LAYOUT_DMR, bits);
        mbe_encoderDestroy(e);
    }));

    std::printf("\n%-58s %10s %12s\n", "path", "us/frame", "x real time");
    for (const Result& r : results)
        std::printf("%-58s %10.1f %12.0f\n", r.name.c_str(), r.usPerFrame, 20000.0 / r.usPerFrame);
    if (!jsonOut.empty()) {
        std::ofstream j(jsonOut);
        j << "{\n  \"version\": \"" << mbe_versionString() << "\",\n  \"frames\": " << frames << ",\n  \"results\": {\n";
        for (size_t i = 0; i < results.size(); ++i)
            j << "    \"" << results[i].name << "\": " << results[i].usPerFrame << (i + 1 < results.size() ? ",\n" : "\n");
        j << "  }\n}\n";
    }
    return 0;
}
