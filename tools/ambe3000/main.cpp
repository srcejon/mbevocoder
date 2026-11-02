// SPDX-License-Identifier: GPL-3.0-or-later
//
// ambe3000: drive an AMBE-3000R (ThumbDV / DV3000U class dongle) as a reference
// encoder/decoder.
//
//   ambe3000 --port COM3 identify
//   ambe3000 --port COM3 --rate dstar|ambe2450|raw2450 encode   in.wav   out.dvsi
//   ambe3000 --port COM3 --rate ...                    decode   in.dvsi  out.wav
//   ambe3000 --port COM3 --rate ...                    loopback in.wav   out.dvsi out.wav
//
// .dvsi files are raw channel frames as returned by the AMBE-3000R: rateBytes() bytes per
// frame (9 for 72-bit rates, 7 for raw 49-bit), MSB first, DVSI canonical bit order.
// A JSON sidecar (<out>.json) records rate, AMBE-3000R identity and frame counts.
//
// Encoding appends --flush silent frames (default 4) so the AMBE-3000R's ~52 ms algorithmic
// delay does not truncate the tail; all output frames are kept, so the .dvsi has
// input_frames + flush frames. Alignment is the consumer's job.

#include "ambe3000.hpp"
#include "serial_port.hpp"
#include "wav.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace ambe3000;

namespace {

struct Options {
    std::string port;
    unsigned baud = 460800;
    Rate rate = Rate::Ambe2450;
    bool rateGiven = false;
    unsigned flush = 4;
    unsigned depth = 2;
    bool noReset = false;
    int ecmode = -1, dcmode = -1, cmode = -1;
    std::string lostFile;   // decode: text file of frame indices to mark LOST_FRAME (DCMODE_IN bit 2)
    std::vector<std::vector<uint8_t>> ctl;   // raw control packets sent after configure(), in order   // PKT_ECMODE / PKT_DCMODE overrides applied after configure()
    std::string cmd;
    std::vector<std::string> args;
};

void usage()
{
    std::fprintf(stderr,
        "usage: ambe3000 --port <port> [--baud N] [--rate dstar|ambe2450|raw2450] [--flush N] [--depth N] [--no-reset] [--lost frames.txt] <command> ...\n"
        "  identify\n"
        "  encode   <in.wav> <out.dvsi>\n"
        "  decode   <in.dvsi> <out.wav>\n"
        "  loopback <in.wav> <out.dvsi> <out.wav>\n");
}

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& v) { if (i + 1 >= argc) return false; v = argv[++i]; return true; };
        std::string v;
        if (a == "--port") { if (!next(o.port)) return false; }
        else if (a == "--baud") { if (!next(v)) return false; o.baud = static_cast<unsigned>(std::stoul(v)); }
        else if (a == "--rate") { if (!next(v) || !parseRate(v, o.rate)) return false; o.rateGiven = true; }
        else if (a == "--flush") { if (!next(v)) return false; o.flush = static_cast<unsigned>(std::stoul(v)); }
        else if (a == "--depth") { if (!next(v)) return false; o.depth = static_cast<unsigned>(std::stoul(v)); }
        else if (a == "--no-reset") { o.noReset = true; }
        else if (a == "--ecmode") { if (!next(v)) return false; o.ecmode = static_cast<int>(std::stoul(v, nullptr, 0)); }
        else if (a == "--ctl") {
            if (!next(v)) return false;
            std::vector<uint8_t> pkt;
            for (size_t k = 0; k + 1 < v.size(); k += 2) pkt.push_back(static_cast<uint8_t>(std::stoul(v.substr(k, 2), nullptr, 16)));
            o.ctl.push_back(pkt);
        }
        else if (a == "--cmode") { if (!next(v)) return false; o.cmode = static_cast<int>(std::stoul(v, nullptr, 0)); }
        else if (a == "--dcmode") { if (!next(v)) return false; o.dcmode = static_cast<int>(std::stoul(v, nullptr, 0)); }
        else if (a == "--lost") { if (!next(o.lostFile)) return false; }
        else if (a.rfind("--", 0) == 0) { return false; }
        else if (o.cmd.empty()) { o.cmd = a; }
        else { o.args.push_back(a); }
    }
    return !o.port.empty() && !o.cmd.empty();
}

std::vector<Pcm> toFrames(const std::vector<int16_t>& s, unsigned flush)
{
    std::vector<Pcm> frames;
    size_t n = (s.size() + SAMPLES_PER_FRAME - 1) / SAMPLES_PER_FRAME;
    frames.resize(n + flush);
    for (size_t i = 0; i < n; ++i)
        for (unsigned k = 0; k < SAMPLES_PER_FRAME; ++k) {
            size_t idx = i * SAMPLES_PER_FRAME + k;
            frames[i][k] = idx < s.size() ? s[idx] : int16_t(0);
        }
    for (size_t i = n; i < n + flush; ++i) frames[i].fill(0);
    return frames;
}

void writeFrames(const std::string& path, const std::vector<std::vector<uint8_t>>& frames)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot create " + path);
    for (const auto& fr : frames) f.write(reinterpret_cast<const char*>(fr.data()), static_cast<std::streamsize>(fr.size()));
}

std::vector<std::vector<uint8_t>> readFrames(const std::string& path, unsigned bytesPerFrame)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (all.size() % bytesPerFrame != 0)
        std::fprintf(stderr, "warning: %s size %zu is not a multiple of %u bytes\n", path.c_str(), all.size(), bytesPerFrame);
    std::vector<std::vector<uint8_t>> frames;
    for (size_t i = 0; i + bytesPerFrame <= all.size(); i += bytesPerFrame)
        frames.emplace_back(all.begin() + static_cast<long>(i), all.begin() + static_cast<long>(i + bytesPerFrame));
    return frames;
}

void writeSidecar(const std::string& path, const Options& o, const std::string& product, const std::string& version,
                  const std::string& source, size_t inputFrames, size_t outputFrames)
{
    std::ofstream f(path + ".json");
    f << "{\n"
      << "  \"format\": \"dvsi-channel\",\n"
      << "  \"rate\": \"" << rateName(o.rate) << "\",\n"
      << "  \"bits_per_frame\": " << rateBits(o.rate) << ",\n"
      << "  \"bytes_per_frame\": " << rateBytes(o.rate) << ",\n"
      << "  \"product\": \"" << product << "\",\n"
      << "  \"version\": \"" << version << "\",\n"
      << "  \"source\": \"" << source << "\",\n"
      << "  \"input_frames\": " << inputFrames << ",\n"
      << "  \"flush_frames\": " << o.flush << ",\n"
      << "  \"output_frames\": " << outputFrames << "\n"
      << "}\n";
}

std::string baseName(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    if (!parse(argc, argv, o)) { usage(); return 2; }

    try {
        SerialPort port;
        port.open(o.port, o.baud);
        Device dev(port);

        if (!o.noReset) {
            if (!dev.reset()) {
                // Older firmware or a dongle without a reset line may not answer; carry on.
                std::fprintf(stderr, "warning: no PKT_READY after PKT_RESET, continuing\n");
            }
        }
        std::string product = dev.productId();
        std::string version = dev.versionString();

        if (o.cmd == "identify") {
            std::printf("product: %s\nversion: %s\nport: %s @ %u\n", product.c_str(), version.c_str(), o.port.c_str(), o.baud);
            return product.rfind("AMBE3000", 0) == 0 ? 0 : 1;
        }

        if (!o.rateGiven) { std::fprintf(stderr, "--rate is required for %s\n", o.cmd.c_str()); return 2; }
        dev.configure(o.rate);
        if (o.ecmode >= 0) dev.setMode(true, static_cast<uint16_t>(o.ecmode));
        if (o.dcmode >= 0) dev.setMode(false, static_cast<uint16_t>(o.dcmode));
        if (o.cmode >= 0) dev.setSpeechCmode(o.cmode);
        for (const auto& pkt : o.ctl) {
            Packet r = dev.control(pkt);
            std::fprintf(stderr, "ctl %02x -> type %02x payload", pkt[0], r.type);
            for (uint8_t b : r.payload) std::fprintf(stderr, " %02x", b);
            std::fputc('\n', stderr);
        }
        std::fprintf(stderr, "%s %s, rate %s\n", product.c_str(), version.c_str(), rateName(o.rate));

        auto progress = [](const char* what, size_t total) {
            return [what, total](size_t n) { std::fprintf(stderr, "\r%s %zu/%zu", what, n, total); if (n == total) std::fputc('\n', stderr); };
        };
        using clock = std::chrono::steady_clock;

        if (o.cmd == "encode" || o.cmd == "loopback") {
            if (o.args.size() < 2) { usage(); return 2; }
            wav::Audio in = wav::read(o.args[0]);
            if (in.sampleRate != 8000) { std::fprintf(stderr, "%s: sample rate must be 8000 Hz (is %u)\n", o.args[0].c_str(), in.sampleRate); return 1; }
            auto frames = toFrames(in.samples, o.flush);
            auto t0 = clock::now();
            auto enc = dev.encode(frames, o.depth, progress("encode", frames.size()));
            double secs = std::chrono::duration<double>(clock::now() - t0).count();
            writeFrames(o.args[1], enc);
            writeSidecar(o.args[1], o, product, version, baseName(o.args[0]), frames.size() - o.flush, enc.size());
            {   // the ECMODE_OUT word per frame (VOICE_ACTIVE, TONE_FRAME), when the AMBE-3000R sent one
                const auto& cm = dev.cmodeOut();
                bool any = false;
                for (int v : cm) if (v >= 0) any = true;
                if (any) {
                    std::ofstream f(o.args[1] + ".cmode");
                    for (int v : cm) f << v << std::endl;
                }
            }
            std::fprintf(stderr, "encoded %zu frames in %.1f s (%.1fx real time)\n", enc.size(), secs, (static_cast<double>(enc.size()) * 0.02) / secs);
            if (o.cmd == "loopback") {
                if (o.args.size() < 3) { usage(); return 2; }
                t0 = clock::now();
                auto pcm = dev.decode(enc, rateBits(o.rate), o.depth, progress("decode", enc.size()));
                secs = std::chrono::duration<double>(clock::now() - t0).count();
                wav::Audio out; out.sampleRate = 8000;
                for (const auto& fr : pcm) out.samples.insert(out.samples.end(), fr.begin(), fr.end());
                wav::write(o.args[2], out);
                std::fprintf(stderr, "decoded %zu frames in %.1f s (%.1fx real time)\n", pcm.size(), secs, (static_cast<double>(pcm.size()) * 0.02) / secs);
            }
            return 0;
        }

        if (o.cmd == "decode") {
            if (o.args.size() < 2) { usage(); return 2; }
            auto frames = readFrames(o.args[0], rateBytes(o.rate));
            if (!o.lostFile.empty()) {
                std::ifstream lf(o.lostFile);
                // DCMODE_IN keeps its value until changed, so every frame carries the field
                // (0 for the frames that were received) once any frame is marked lost
                std::vector<int> cmodes(frames.size(), 0);
                long idx;
                while (lf >> idx) if (idx >= 0 && static_cast<size_t>(idx) < cmodes.size()) cmodes[static_cast<size_t>(idx)] = 0x0004;
                dev.setFrameCmodes(cmodes);
            }
            auto t0 = clock::now();
            auto pcm = dev.decode(frames, rateBits(o.rate), o.depth, progress("decode", frames.size()));
            double secs = std::chrono::duration<double>(clock::now() - t0).count();
            wav::Audio out; out.sampleRate = 8000;
            for (const auto& fr : pcm) out.samples.insert(out.samples.end(), fr.begin(), fr.end());
            wav::write(o.args[1], out);
            std::fprintf(stderr, "decoded %zu frames in %.1f s (%.1fx real time)\n", pcm.size(), secs, (static_cast<double>(pcm.size()) * 0.02) / secs);
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
