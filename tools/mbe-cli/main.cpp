// SPDX-License-Identifier: GPL-3.0-or-later
//
// mbe-cli: command-line front end for mbevocoder.
//
//   mbe-cli decode --rate dstar|ambe2450|imbe4400|imbe7200 [--layout dstar|dmr|p25p2|p25p1|ysf_vw]
//                  [--lsb-first|--msb-first] [--errors] [--lost frames.txt] [--gain X (default 1)] in.dvsi out.wav
//   mbe-cli encode --rate ambe2450|dstar|raw2450|imbe4400|imbe7200 [--gain-adjust X] [--floor X] [--limiter]
//                  [--flush N] [--parms out.txt] [--tone-detect|--no-tone-detect] [--dtx] [--noise-suppress]
//                  [--echo-cancel far.wav] [--flags f.txt] [--fe-out fe.wav]
//                  [--vt a,b,...] [--oracle-file f] (the last two for tools/quality experiments) in.wav out.dvsi
//   mbe-cli parms  --rate ambe2450|dstar|imbe7200 in.dvsi out.txt   quantiser indices / decoded model per frame
//   mbe-cli frbits --rate ambe2450|dstar|imbe7200 in.dvsi out.txt   mbelib's frame arrays as 0/1 text
//   mbe-cli frpack --rate ambe2450|dstar|imbe7200 in.txt out.dvsi   the inverse of frbits
//   mbe-cli dpack  --rate ambe2450|dstar in.txt out.dvsi            parameter bits (49 per line) -> channel bytes
//   mbe-cli dunpack --rate ambe2450|dstar in.dvsi out.txt           and back (FEC-decoded)
//   mbe-cli frames --rate ambe2450|dstar in.txt out.dvsi            frames from quantiser indices (b0..b8 per line)
//   mbe-cli transcode --from RATE --to RATE in.dvsi out.dvsi        parameter-domain transcoding
//   mbe-cli tone --rate ambe2450|dstar|raw2450 --id N [--amp A] [--frames N] out.dvsi
//
// decode: reads packed 72-bit frames (9 bytes each; AMBE-3000R .dvsi captures by default:
// D-STAR LSB first in the D-STAR layout, AMBE+2 MSB first in the DMR layout), decodes them
// and writes 8 kHz 16-bit mono WAV. --errors prints per-frame error counts and flags.
// encode: reads 8 kHz mono WAV and writes frames in the AMBE-3000R channel-packet format
// (9 bytes DMR layout MSB first for ambe2450, 7 bytes raw order for raw2450, 11 bytes of
// 88 bits MSB first for imbe4400) plus --flush trailing silent frames (default 4, matching
// the AMBE-3000R captures). --parms writes one line per frame with the quantiser indices.

#include <mbevocoder/mbevocoder.h>

#include "../ambe3000/wav.hpp"
#include "encoder/encoder_internal.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace {

void usage()
{
    std::fprintf(stderr, "usage: mbe-cli decode --rate dstar|ambe2450|imbe4400|imbe7200 [--layout dstar|dmr|p25p2|p25p1|ysf_vw] [--lsb-first|--msb-first] [--errors] [--lost frames.txt] [--gain X (default 1)] in.dvsi out.wav\n"
                         "       mbe-cli encode --rate ambe2450|dstar|raw2450|imbe4400|imbe7200 [--gain-adjust X] [--floor X] [--limiter] [--flush N] [--parms out.txt]\n"
                         "                      [--tone-detect|--no-tone-detect] [--dtx] [--noise-suppress] [--echo-cancel far.wav] [--flags f.txt] [--fe-out fe.wav] in.wav out.dvsi\n"
                         "       mbe-cli parms|frbits --rate ambe2450|dstar|imbe7200 in.dvsi out.txt   (quantiser indices or decoded model / mbelib frame arrays)\n"
                         "       mbe-cli frpack --rate ambe2450|dstar|imbe7200 in.txt out.dvsi   (frame arrays as frbits writes them -> channel bytes)\n"
                         "       mbe-cli dpack|dunpack --rate ambe2450|dstar in out   (parameter bits, 49 per line <-> channel bytes)\n"
                         "       mbe-cli frames --rate ambe2450|dstar in.txt out.dvsi   (frames from b0..b8 per line)\n"
                         "       mbe-cli transcode --from RATE --to RATE in.dvsi out.dvsi   (RATE: ambe2450|dstar|raw2450|imbe4400|imbe7200)\n"
                         "       mbe-cli tone --rate ambe2450|dstar|raw2450 --id N [--amp A] [--frames N] out.dvsi\n");
}

int encodeCmd(const std::string& rate, const std::string& in, const std::string& out, float gainAdjust, float floorV, bool limiter,
              unsigned flush, const std::string& parmsPath, const std::string& voicingThresholds, const std::string& oracleFile, int toneDetect,
              int dtx, const std::string& flagsPath, int noiseSuppress, const std::string& farEndPath, const std::string& feOutPath)
{
    mbe_rate r;
    if (rate == "ambe2450" || rate == "dmr") r = MBE_RATE_AMBE3600x2450;
    else if (rate == "dstar") r = MBE_RATE_AMBE3600x2400;
    else if (rate == "raw2450") r = MBE_RATE_AMBE2450;
    else if (rate == "imbe4400") r = MBE_RATE_IMBE4400;
    else if (rate == "imbe7200") r = MBE_RATE_IMBE7200x4400;
    else { usage(); return 2; }
    wav::Audio audio = wav::read(in);
    if (audio.sampleRate != 8000) { std::fprintf(stderr, "%s: sample rate must be 8000 Hz\n", in.c_str()); return 1; }
    mbe_encoder_options opts;
    mbe_encoderDefaultOptions(&opts);
    opts.gain_adjust = gainAdjust;
    opts.amplitude_floor = floorV;
    opts.limiter = limiter ? 1 : 0;
    if (toneDetect >= 0) opts.tone_detect = toneDetect;
    opts.dtx = dtx;
    opts.noise_suppress = noiseSuppress;
    wav::Audio farEnd;
    if (!farEndPath.empty()) {
        farEnd = wav::read(farEndPath);
        if (farEnd.sampleRate != 8000) { std::fprintf(stderr, "%s: sample rate must be 8000 Hz\n", farEndPath.c_str()); return 1; }
        opts.echo_cancel = 1;
    }
    wav::Audio feOut;
    feOut.sampleRate = 8000;
    if (!voicingThresholds.empty()) {
        // --vt a,b,c,d,e,f,g,h : per-band voicing threshold multipliers (experiments)
        size_t pos = 0;
        for (int k = 0; k < 8 && pos <= voicingThresholds.size(); ++k) {
            size_t comma = voicingThresholds.find(',', pos);
            opts.voicing_threshold[k] = std::stof(voicingThresholds.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    mbe_encoder* enc = mbe_encoderCreate(r, &opts);
    if (!enc) { std::fprintf(stderr, "encoder for %s not available\n", rate.c_str()); return 1; }
    // --oracle-file: one line per frame "period b1 gamma L log2Ml[1..L]" replacing parts of
    // the analysis with reference values (period 0, b1 -1, gamma nan, L 0 = not forced; a
    // missing line forces nothing). Experiments only: tools/quality/frontend_oracle.py.
    struct Forced { float period; mbe::QuantizerOverride ov; bool any; };
    std::vector<Forced> forced;
    if (!oracleFile.empty()) {
        std::ifstream pf(oracleFile);
        std::string line;
        while (std::getline(pf, line)) {
            std::istringstream ls(line);
            Forced f{};
            std::string gammaS;
            int L = 0;
            if (!(ls >> f.period >> f.ov.b1 >> gammaS >> L)) continue;
            if (gammaS != "nan") { f.ov.gamma = std::stof(gammaS); f.ov.haveGamma = true; }
            if (L > 0 && L <= NUM_HARMS_MAX_OVERRIDE) {
                float w0;
                ls >> w0;
                f.ov.L = L; f.ov.w0 = w0;
                for (int l = 1; l <= L; ++l) ls >> f.ov.log2Ml[l];
            }
            // IMBE: an optional trailing "V<w0> <0/1 per harmonic>" gives the reference voicing
            std::string vtok;
            if (ls >> vtok && vtok.size() > 1 && vtok[0] == 'V') {
                std::string bits;
                ls >> bits;
                f.ov.voicedW0 = std::stof(vtok.substr(1));   // its own grid; w0 stays the amplitudes'
                f.ov.voicedL = static_cast<int>(bits.size()) < NUM_HARMS_MAX_OVERRIDE ? static_cast<int>(bits.size()) : NUM_HARMS_MAX_OVERRIDE;
                for (int l = 1; l <= f.ov.voicedL; ++l) f.ov.voiced[l] = bits[static_cast<size_t>(l - 1)] == '1';
            }
            f.any = f.ov.b1 >= 0 || f.ov.haveGamma || f.ov.L > 0;
            forced.push_back(f);
        }
    }
    std::ofstream f(out, std::ios::binary);
    std::ofstream parms, flags;
    if (!parmsPath.empty()) parms.open(parmsPath);
    if (!flagsPath.empty()) flags.open(flagsPath);
    size_t total = (audio.samples.size() + 159) / 160 + flush;
    long frames = 0, rangeErrors = 0;
    for (size_t i = 0; i < total; ++i) {
        short pcm[160] = {0};
        for (int k = 0; k < 160; ++k) {
            size_t idx = i * 160 + k;
            pcm[k] = idx < audio.samples.size() ? audio.samples[idx] : short(0);
        }
        int b[9] = {0};
        char d[88];
        if (i < forced.size()) {
            mbe::encoderForcePitch(enc, forced[i].period);
            mbe::encoderForceModel(enc, forced[i].any ? &forced[i].ov : nullptr);
        }
        if (opts.echo_cancel) {
            short far[160] = {0};
            for (int k = 0; k < 160; ++k) {
                size_t idx = i * 160 + k;
                far[k] = idx < farEnd.samples.size() ? farEnd.samples[idx] : short(0);
            }
            mbe_encoderSetEchoReference(enc, far);
        }
        int ret = mbe_encoderEncodeParms(enc, pcm, d, b);
        if (!feOutPath.empty()) {
            short fe[160];
            mbe::encoderFrontEndOutput(enc, fe);
            feOut.samples.insert(feOut.samples.end(), fe, fe + 160);
        }
        if (ret == MBE_STATUS_ENCODE_RANGE) {
            // out of codec range: repeat the last parameters (encoder state is unchanged)
            ++rangeErrors;
            ret = 0;
        } else if (ret < 0) { std::fprintf(stderr, "frame %zu: status %d\n", i, ret); return 1; }
        if (flags.is_open()) flags << mbe_encoderFrameFlags(enc) << std::endl;
        uint8_t bytes[18];
        if (r == MBE_RATE_IMBE4400) mbe_packBits(d, 88, bytes, 1);
        else if (r == MBE_RATE_IMBE7200x4400) {
            char fr[8][23], bits[144];
            mbe_encodeImbe7200x4400Frame(d, fr);
            mbe_imbeFrameToBits(MBE_LAYOUT_P25P1, fr, bits);
            mbe_packBits(bits, 144, bytes, 1);
        }
        else if (r == MBE_RATE_AMBE2450) mbe_ambe2450DataToDvsi(d, bytes);
        else if (r == MBE_RATE_AMBE3600x2400) {
            char fr[4][24], bits[72];
            mbe_encodeAmbe3600x2400Frame(d, fr);
            mbe_ambeFrameToBits(MBE_LAYOUT_DSTAR, fr, bits);
            mbe_packBits(bits, 72, bytes, 0);
        } else {
            char fr[4][24], bits[72];
            mbe_encodeAmbe3600x2450Frame(d, fr);
            mbe_ambeFrameToBits(MBE_LAYOUT_DMR, fr, bits);
            mbe_packBits(bits, 72, bytes, 1);
        }
        size_t nbytes = r == MBE_RATE_IMBE4400 ? 11 : (r == MBE_RATE_IMBE7200x4400 ? 18 : (r == MBE_RATE_AMBE2450 ? 7 : 9));
        f.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(nbytes));
        if (parms.is_open()) {
            // b indices, then "A w0 L" followed by L entries of signed log2 amplitude
            // (negative = unvoiced) from the unquantised analysis model.
            int n = (r == MBE_RATE_IMBE4400 || r == MBE_RATE_IMBE7200x4400) ? 8 : 9;
            for (int k = 0; k < n; ++k) parms << b[k] << ' ';
            mbe_parms model;
            mbe_encoderAnalysis(enc, &model);
            parms << "A " << model.w0 << ' ' << model.L;
            for (int l = 1; l <= model.L; ++l) parms << ' ' << (model.Vl[l] ? model.log2Ml[l] : -model.log2Ml[l]);
            // "V" then per harmonic: voicing ratio (dk/threshold), voiced-hypothesis and
            // unvoiced-hypothesis amplitude
            float ratio[57], sv[57], su[57];
            if (mbe_encoderVoicingStats(enc, ratio, sv, su) > 0) {
                parms << " E " << ratio[0] << " V";
                for (int l = 1; l <= model.L; ++l) parms << ' ' << ratio[l] << ' ' << sv[l] << ' ' << su[l];
            }
            parms << '\n';
        }
        ++frames;
    }
    if (!feOutPath.empty()) wav::write(feOutPath, feOut);
    mbe_encoderDestroy(enc);
    std::fprintf(stderr, "%ld frames (%u flush), %ld out-of-range frames, wrote %s\n", frames, flush, rangeErrors, out.c_str());
    return 0;
}

// Frame container conventions shared by every command: bytes per frame and the mapping
// between the packed frame and the parameter bits.
bool parseRate(const std::string& name, mbe_rate& r)
{
    if (name == "ambe2450" || name == "dmr") r = MBE_RATE_AMBE3600x2450;
    else if (name == "dstar") r = MBE_RATE_AMBE3600x2400;
    else if (name == "raw2450") r = MBE_RATE_AMBE2450;
    else if (name == "imbe4400") r = MBE_RATE_IMBE4400;
    else if (name == "imbe7200") r = MBE_RATE_IMBE7200x4400;
    else return false;
    return true;
}

size_t frameBytes(mbe_rate r)
{
    switch (r) {
        case MBE_RATE_AMBE2450: return 7;
        case MBE_RATE_IMBE4400: return 11;
        case MBE_RATE_IMBE7200x4400: return 18;
        default: return 9;
    }
}

// Packed frame -> parameter bits (FEC decoded); returns the corrected error count or -1.
int frameToData(mbe_rate r, const uint8_t* bytes, char* d)
{
    mbe_process_result res;
    switch (r) {
        case MBE_RATE_AMBE2450: mbe_dvsiToAmbe2450Data(bytes, d); return 0;
        case MBE_RATE_IMBE4400: mbe_unpackBits(bytes, 88, d, 1); return 0;
        case MBE_RATE_IMBE7200x4400: {
            char bits[144], fr[8][23];
            mbe_unpackBits(bytes, 144, bits, 1);
            mbe_imbeFrameFromBits(MBE_LAYOUT_P25P1, bits, fr);
            return mbe_decodeImbe7200x4400Frame(fr, d, &res) < 0 ? -1 : res.total_errors;
        }
        case MBE_RATE_AMBE3600x2400: {
            char bits[72], fr[4][24];
            mbe_unpackBits(bytes, 72, bits, 0);
            mbe_ambeFrameFromBits(MBE_LAYOUT_DSTAR, bits, fr);
            return mbe_decodeAmbe3600x2400Frame(fr, d, &res) < 0 ? -1 : res.total_errors;
        }
        default: {
            char bits[72], fr[4][24];
            mbe_unpackBits(bytes, 72, bits, 1);
            mbe_ambeFrameFromBits(MBE_LAYOUT_DMR, bits, fr);
            return mbe_decodeAmbe3600x2450Frame(fr, d, &res) < 0 ? -1 : res.total_errors;
        }
    }
}

void dataToFrame(mbe_rate r, const char* d, uint8_t* bytes)
{
    switch (r) {
        case MBE_RATE_AMBE2450: mbe_ambe2450DataToDvsi(d, bytes); break;
        case MBE_RATE_IMBE4400: mbe_packBits(d, 88, bytes, 1); break;
        case MBE_RATE_IMBE7200x4400: {
            char fr[8][23], bits[144];
            mbe_encodeImbe7200x4400Frame(d, fr);
            mbe_imbeFrameToBits(MBE_LAYOUT_P25P1, fr, bits);
            mbe_packBits(bits, 144, bytes, 1);
            break;
        }
        case MBE_RATE_AMBE3600x2400: {
            char fr[4][24], bits[72];
            mbe_encodeAmbe3600x2400Frame(d, fr);
            mbe_ambeFrameToBits(MBE_LAYOUT_DSTAR, fr, bits);
            mbe_packBits(bits, 72, bytes, 0);
            break;
        }
        default: {
            char fr[4][24], bits[72];
            mbe_encodeAmbe3600x2450Frame(d, fr);
            mbe_ambeFrameToBits(MBE_LAYOUT_DMR, fr, bits);
            mbe_packBits(bits, 72, bytes, 1);
            break;
        }
    }
}

int transcodeCmd(const std::string& fromName, const std::string& toName, const std::string& in, const std::string& out)
{
    mbe_rate from, to;
    if (!parseRate(fromName, from) || !parseRate(toName, to)) { usage(); return 2; }
    std::ifstream f(in, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::ofstream o(out, std::ios::binary);
    mbe_transcoder* t = mbe_transcoderCreate(from, to);
    if (!t) { std::fprintf(stderr, "transcoder %s -> %s not available\n", fromName.c_str(), toName.c_str()); return 1; }
    const size_t nin = frameBytes(from), nout = frameBytes(to);
    long frames = 0, tones = 0, repeats = 0, errors = 0;
    for (size_t i = 0; i + nin <= data.size(); i += nin, ++frames) {
        char din[88], dout[88];
        int e = frameToData(from, &data[i], din);
        if (e < 0) { ++errors; continue; }
        int kind = mbe_transcoderProcess(t, din, dout);
        if (kind < 0) { std::fprintf(stderr, "frame %ld: status %d\n", frames, kind); return 1; }
        if (kind == MBE_TRANSCODE_TONE) ++tones;
        if (kind == MBE_TRANSCODE_REPEAT) ++repeats;
        uint8_t bytes[18];
        dataToFrame(to, dout, bytes);
        o.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(nout));
    }
    mbe_transcoderDestroy(t);
    std::fprintf(stderr, "%ld frames (%ld tone, %ld repeated, %ld unreadable), wrote %s\n", frames, tones, repeats, errors, out.c_str());
    return 0;
}

int toneCmd(const std::string& rateName, int id, int amp, unsigned frames, const std::string& out)
{
    mbe_rate r;
    if (!parseRate(rateName, r) || r == MBE_RATE_IMBE4400 || r == MBE_RATE_IMBE7200x4400) { usage(); return 2; }
    mbe_encoder* e = mbe_encoderCreate(r, nullptr);
    std::ofstream o(out, std::ios::binary);
    for (unsigned i = 0; i < frames; ++i) {
        uint8_t bytes[9];
        int ret = mbe_encoderToneDvsi(e, id, amp, bytes);
        if (ret < 0) { std::fprintf(stderr, "tone id %d / amplitude %d rejected (%d)\n", id, amp, ret); return 1; }
        o.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(frameBytes(r)));
    }
    mbe_encoderDestroy(e);
    float f1, f2;
    mbe_toneFrequencies(id, &f1, &f2);
    std::fprintf(stderr, "%u tone frames id %d (%.2f/%.2f Hz) amplitude %d, wrote %s\n", frames, id, f1, f2, amp, out.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string cmd, rate, layoutName, in, out, parmsPath, fromName, toName, voicingThresholds, oracleFile;
    int toneDetect = -1;   // -1: library default
    int dtx = 0, noiseSuppress = 0;
    std::string flagsPath;   // per-frame mbe_encoderFrameFlags() written here
    std::string farEndPath;  // --echo-cancel: the far-end wav played out, aligned with the input
    std::string feOutPath;   // --fe-out: the input after the front end, as a wav
    int bitOrder = -1, toneId = 32, toneAmp = MBE_TONE_AMPLITUDE_DEFAULT;
    unsigned toneFrames = 50;
    bool printErrors = false, limiter = false;
    std::string lostPath;   // decode: frame indices to treat as lost (mbe_processLostFramef)
    // decode: multiplier on the float output before the 16-bit conversion (x7 and a clip at
    // 95 % of full scale). The default 1 is the library's level, the AMBE-3000R's (the
    // neo-compat build keeps mbelib's, 16 dB above it: decode it with --gain 0.125).
    float outGain = 1.0f;
    float gainAdjust = 0.0f, floorV = 0.0f;
    unsigned flush = 4;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = argv[++i];
        else if (a == "--gain-adjust" && i + 1 < argc) gainAdjust = std::stof(argv[++i]);
        else if (a == "--floor" && i + 1 < argc) floorV = std::stof(argv[++i]);
        else if (a == "--limiter") limiter = true;
        else if (a == "--vt" && i + 1 < argc) voicingThresholds = argv[++i];
        else if (a == "--oracle-file" && i + 1 < argc) oracleFile = argv[++i];
        else if (a == "--tone-detect") toneDetect = 1;
        else if (a == "--no-tone-detect") toneDetect = 0;
        else if (a == "--dtx") dtx = 1;
        else if (a == "--flags" && i + 1 < argc) flagsPath = argv[++i];
        else if (a == "--noise-suppress") noiseSuppress = 1;
        else if (a == "--echo-cancel" && i + 1 < argc) farEndPath = argv[++i];
        else if (a == "--fe-out" && i + 1 < argc) feOutPath = argv[++i];
        else if (a == "--flush" && i + 1 < argc) flush = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--parms" && i + 1 < argc) parmsPath = argv[++i];
        else if (a == "--layout" && i + 1 < argc) layoutName = argv[++i];
        else if (a == "--from" && i + 1 < argc) fromName = argv[++i];
        else if (a == "--to" && i + 1 < argc) toName = argv[++i];
        else if (a == "--id" && i + 1 < argc) toneId = std::stoi(argv[++i]);
        else if (a == "--amp" && i + 1 < argc) toneAmp = std::stoi(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) toneFrames = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--lsb-first") bitOrder = 0;
        else if (a == "--msb-first") bitOrder = 1;
        else if (a == "--errors") printErrors = true;
        else if (a == "--lost" && i + 1 < argc) lostPath = argv[++i];
        else if (a == "--gain" && i + 1 < argc) outGain = std::stof(argv[++i]);
        else if (a.rfind("--", 0) == 0) { usage(); return 2; }
        else if (cmd.empty()) cmd = a;
        else if (in.empty()) in = a;
        else out = a;
    }
    if (cmd == "encode" && !in.empty() && !out.empty()) return encodeCmd(rate, in, out, gainAdjust, floorV, limiter, flush, parmsPath, voicingThresholds, oracleFile, toneDetect, dtx, flagsPath, noiseSuppress, farEndPath, feOutPath);
    if (cmd == "transcode" && !in.empty() && !out.empty()) return transcodeCmd(fromName, toName, in, out);
    if (cmd == "tone" && !in.empty() && out.empty()) return toneCmd(rate, toneId, toneAmp, toneFrames, in);
    if (cmd == "frames" && !in.empty() && !out.empty() && (rate == "ambe2450" || rate == "dstar")) {
        // Synthesise frames from a text file of quantiser indices (b0..b8 per line) into the
        // AMBE-3000R capture format. For decoder probing experiments.
        const bool dstar = rate == "dstar";
        std::ifstream fin(in);
        std::ofstream fout(out, std::ios::binary);
        int b[9];
        long n = 0;
        while (fin >> b[0] >> b[1] >> b[2] >> b[3] >> b[4] >> b[5] >> b[6] >> b[7] >> b[8]) {
            char d[49], fr[4][24], bits[72];
            uint8_t bytes[9];
            if ((dstar ? mbe_packAmbe2400Fields : mbe_packAmbe2450Fields)(b, d) < 0) { std::fprintf(stderr, "bad indices on line %ld\n", n + 1); return 1; }
            (dstar ? mbe_encodeAmbe3600x2400Frame : mbe_encodeAmbe3600x2450Frame)(d, fr);
            mbe_ambeFrameToBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, fr, bits);
            mbe_packBits(bits, 72, bytes, dstar ? 0 : 1);
            fout.write(reinterpret_cast<const char*>(bytes), 9);
            ++n;
        }
        std::fprintf(stderr, "%ld frames written\n", n);
        return 0;
    }
    if (cmd == "frbits" && !in.empty() && !out.empty() && rate == "imbe7200") {
        // The same for IMBE 7200x4400 (P25 Phase 1 layout): the 184 bits of mbelib's imbe_fr[8][23].
        std::ifstream fin(in, std::ios::binary);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        std::ofstream fout(out);
        for (size_t i = 0; i + 18 <= data.size(); i += 18) {
            char bits[144], fr[8][23];
            mbe_unpackBits(&data[i], 144, bits, 1);
            mbe_imbeFrameFromBits(MBE_LAYOUT_P25P1, bits, fr);
            for (int r = 0; r < 8; ++r) for (int c = 0; c < 23; ++c) fout << (fr[r][c] ? '1' : '0');
            fout << std::endl;
        }
        return 0;
    }
    if (cmd == "frbits" && !in.empty() && !out.empty() && (rate == "ambe2450" || rate == "dstar")) {
        // Dump each frame of an AMBE-3000R capture as the 96 bits of mbelib's ambe_fr[4][24] (one
        // line of 0/1 per frame, row-major): for feeding other mbelib builds (tools/quality/mbelib1_decode.py).
        const bool dstar = rate == "dstar";
        std::ifstream fin(in, std::ios::binary);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        std::ofstream fout(out);
        for (size_t i = 0; i + 9 <= data.size(); i += 9) {
            char bits[72], fr[4][24];
            mbe_unpackBits(&data[i], 72, bits, dstar ? 0 : 1);
            mbe_ambeFrameFromBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits, fr);
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 24; ++c) fout << (fr[r][c] ? '1' : '0');
            fout << std::endl;
        }
        return 0;
    }
    if (cmd == "dunpack" && !in.empty() && !out.empty() && (rate == "ambe2450" || rate == "dstar")) {
        // The inverse of dpack: channel bytes -> FEC-decoded parameter bits, 49 of 0/1 per line.
        const bool dstar = rate == "dstar";
        std::ifstream fin(in, std::ios::binary);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        std::ofstream fout(out);
        for (size_t i = 0; i + 9 <= data.size(); i += 9) {
            char bits[72], fr[4][24], d[49];
            mbe_unpackBits(&data[i], 72, bits, dstar ? 0 : 1);
            mbe_ambeFrameFromBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits, fr);
            if ((dstar ? mbe_decodeAmbe3600x2400Frame(fr, d, nullptr) : mbe_decodeAmbe3600x2450Frame(fr, d, nullptr)) < 0) return 1;
            for (int k = 0; k < 49; ++k) fout << (d[k] ? '1' : '0');
            fout << std::endl;
        }
        return 0;
    }
    if (cmd == "dpack" && !in.empty() && !out.empty() && (rate == "ambe2450" || rate == "dstar")) {
        // Parameter bits (49 of 0/1 per line, ambe_d order) -> FEC frames -> channel bytes as frpack
        // writes them, for probe streams with any word in them (erasures, invalid tone frames).
        const bool dstar = rate == "dstar";
        std::ifstream fin(in);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::ofstream fout(out, std::ios::binary);
        std::string line;
        while (std::getline(fin, line)) {
            if (line.size() < 49u) continue;
            char d[49], fr[4][24], bits[72];
            uint8_t bytes[9];
            for (int i = 0; i < 49; ++i) d[i] = line[i] == '1';
            if ((dstar ? mbe_encodeAmbe3600x2400Frame(d, fr) : mbe_encodeAmbe3600x2450Frame(d, fr)) < 0) return 1;
            if (mbe_ambeFrameToBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, fr, bits) < 0) return 1;
            mbe_packBits(bits, 72, bytes, dstar ? 0 : 1);
            fout.write(reinterpret_cast<const char*>(bytes), 9);
        }
        return 0;
    }
    if (cmd == "frpack" && !in.empty() && !out.empty() && (rate == "ambe2450" || rate == "dstar" || rate == "imbe7200")) {
        // The inverse of frbits: frame text (one frame of 0/1 per line) -> channel bytes in the
        // capture format (AMBE+2 DMR layout MSB first, D-STAR layout LSB first, IMBE P25 Phase 1
        // MSB first), for frames from other builds (tools/quality/neo_encoders.py).
        const bool imbe = rate == "imbe7200", dstar = rate == "dstar";
        std::ifstream fin(in);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::ofstream fout(out, std::ios::binary);
        std::string line;
        while (std::getline(fin, line)) {
            if (line.size() < (imbe ? 184u : 96u)) continue;
            uint8_t bytes[18];
            if (imbe) {
                char fr[8][23], bits[144];
                for (int i = 0; i < 184; ++i) fr[i / 23][i % 23] = line[i] == '1';
                if (mbe_imbeFrameToBits(MBE_LAYOUT_P25P1, fr, bits) < 0) return 1;
                mbe_packBits(bits, 144, bytes, 1);
                fout.write(reinterpret_cast<const char*>(bytes), 18);
            } else {
                char fr[4][24], bits[72];
                for (int i = 0; i < 96; ++i) fr[i / 24][i % 24] = line[i] == '1';
                if (mbe_ambeFrameToBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, fr, bits) < 0) return 1;
                mbe_packBits(bits, 72, bytes, dstar ? 0 : 1);
                fout.write(reinterpret_cast<const char*>(bytes), 9);
            }
        }
        return 0;
    }
    if (cmd == "parms" && !in.empty() && !out.empty() && (rate == "ambe2450" || rate == "dstar")) {
        // Dump the quantiser indices of every frame in an AMBE-3000R capture (DMR layout MSB first,
        // or D-STAR layout LSB first).
        const bool dstar = rate == "dstar";
        std::ifstream fin(in, std::ios::binary);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        std::ofstream fout(out);
        // Per frame: b0..b8, then the decoded model: w0 L gamma tone, the voicing flags as a
        // 0/1 string, and log2Ml[1..L].
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        for (size_t i = 0; i + 9 <= data.size(); i += 9) {
            char bits[72], fr[4][24], d[49];
            int b[9];
            mbe_unpackBits(&data[i], 72, bits, dstar ? 0 : 1);
            mbe_ambeFrameFromBits(dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR, bits, fr);
            mbe_process_result r;
            (dstar ? mbe_decodeAmbe3600x2400Frame : mbe_decodeAmbe3600x2450Frame)(fr, d, &r);
            (dstar ? mbe_unpackAmbe2400Fields : mbe_unpackAmbe2450Fields)(d, b);
            for (int k = 0; k < 9; ++k) fout << b[k] << ' ';
            int tone = (dstar ? mbe_decodeAmbe2400Parms : mbe_decodeAmbe2450Parms)(d, &cur, &prev);
            // b0..b8, w0, L, gamma, tone, the voicing flags as one 0/1 string (harmonic 1 first),
            // then log2Ml[1..L]
            fout << cur.w0 << ' ' << cur.L << ' ' << cur.gamma << ' ' << tone << ' ';
            for (int l = 1; l <= cur.L; ++l) fout << (cur.Vl[l] ? '1' : '0');
            for (int l = 1; l <= cur.L; ++l) fout << ' ' << cur.log2Ml[l];
            // tone frames: canonical tone id and amplitude id (AD) appended as "T id amp"
            int tId = -1, tAmp = -1;
            if (!dstar && tone == 7) {
                tId = 0; for (int k = 12; k < 20; ++k) tId = (tId << 1) | d[k];
                tAmp = 0; for (int k = 6; k < 12; ++k) tAmp = (tAmp << 1) | d[k];
                tAmp = (tAmp << 1) | d[44];
            } else if (dstar && (tone == 3 || (tone >= 5 && tone <= 122))) {
                static const int t7[8] = {1, 0, 0, 0, 0, 1, 1, 1}, t6[8] = {0, 0, 0, 1, 1, 1, 1, 0}, t5[8] = {0, 0, 1, 0, 1, 1, 0, 1};
                int def = (d[6] << 2) | (d[7] << 1) | d[8];
                int id = (t7[def] << 7) | (t6[def] << 6) | (t5[def] << 5) | (d[9] << 4) | (d[42] << 3) | (d[43] << 2) | (d[10] << 1) | d[11];
                int vol = 0; for (int k = 12; k < 17; ++k) vol = (vol << 1) | d[k];
                vol = (vol << 3) | (d[44] << 2) | (d[45] << 1) | d[17];
                int canon = mbe_toneIdFromDstar(id);
                if (canon >= 0) { tId = canon; tAmp = (vol + 17) / 2; }
            }
            fout << " T " << tId << ' ' << tAmp << '\n';
            mbe_moveMbeParms(&cur, &prev);
        }
        return 0;
    }
    if (cmd == "parms" && !in.empty() && !out.empty() && rate == "imbe7200") {
        // IMBE 7200x4400 frames (18 bytes, P25 Phase 1 layout): per frame w0 L, the voicing
        // flags as a 0/1 string, then Ml[1..L] (the decoded amplitudes before enhancement),
        // for comparison with another decoder's model (tools/quality/jmbe_crosscheck.py --parms).
        std::ifstream fin(in, std::ios::binary);
        if (!fin) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        std::ofstream fout(out);
        mbe_parms cur, prev, enh;
        mbe_initMbeParms(&cur, &prev, &enh);
        for (size_t i = 0; i + 18 <= data.size(); i += 18) {
            char bits[144], fr[8][23], d[88];
            mbe_unpackBits(&data[i], 144, bits, 1);
            mbe_imbeFrameFromBits(MBE_LAYOUT_P25P1, bits, fr);
            mbe_process_result r;
            mbe_decodeImbe7200x4400Frame(fr, d, &r);
            int ret = mbe_decodeImbe4400Parms(d, &cur, &prev);
            fout << ret << ' ' << cur.w0 << ' ' << cur.L << ' ';
            for (int l = 1; l <= cur.L; ++l) fout << (cur.Vl[l] ? '1' : '0');
            for (int l = 1; l <= cur.L; ++l) fout << ' ' << cur.Ml[l];
            fout << '\n';
            mbe_moveMbeParms(&cur, &prev);
        }
        return 0;
    }
    std::set<long> lost;
    if (cmd == "decode" && !lostPath.empty()) {
        std::ifstream lf(lostPath);
        if (!lf) { std::fprintf(stderr, "cannot open %s\n", lostPath.c_str()); return 1; }
        long idx;
        while (lf >> idx) lost.insert(idx);
    }
    if (cmd == "decode" && !in.empty() && !out.empty() && (rate == "imbe4400" || rate == "imbe7200")) {
        // IMBE: imbe4400 = 11 bytes per frame (88 parameter bits, MSB first, 'mbe-cli encode' output);
        // imbe7200 = 18 bytes per frame (144 on-air bits, MSB first) in the P25 Phase 1 layout
        // (or --layout ysf_vw).
        std::ifstream f(in, std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const bool framed = rate == "imbe7200";
        const size_t nbytes = framed ? 18 : 11;
        mbe_layout layout = layoutName == "ysf_vw" ? MBE_LAYOUT_YSF_VW : MBE_LAYOUT_P25P1;
        mbe_parms cur, prev, prevEnh;
        mbe_initMbeParms(&cur, &prev, &prevEnh);
        wav::Audio audio;
        audio.sampleRate = 8000;
        long frames = 0, totalErrors = 0;
        for (size_t i = 0; i + nbytes <= data.size(); i += nbytes, ++frames) {
            char d[88];
            float pcm[160];
            mbe_process_result r;
            // the raw 4400 path hands r to mbe_processImbe4400Dataf, which reads the FEC stage's
            // error counts and flags from it (there is no FEC stage here: zero errors)
            mbe_initProcessResult(&r);
            int status;
            if (lost.count(frames)) {
                status = mbe_processLostFramef(pcm, &r, &cur, &prev, &prevEnh);
            } else if (framed) {
                char bits[144], fr[8][23];
                mbe_unpackBits(&data[i], 144, bits, 1);
                mbe_imbeFrameFromBits(layout, bits, fr);
                status = mbe_processImbe7200x4400Framef(pcm, &r, fr, d, &cur, &prev, &prevEnh);
            } else {
                mbe_unpackBits(&data[i], 88, d, 1);
                status = mbe_processImbe4400Dataf(pcm, &r, d, &cur, &prev, &prevEnh);
            }
            if (status < 0) { std::fprintf(stderr, "frame %ld: status %d\n", frames, status); std::memset(pcm, 0, sizeof pcm); }
            else totalErrors += r.total_errors;
            if (printErrors) {
                char bar[64];
                mbe_formatProcessResult(bar, sizeof bar, &r);
                std::printf("%6ld c0=%d prot=%d total=%d %s\n", frames, r.c0_errors, r.protected_errors, r.total_errors, bar);
            }
            short s[160];
            for (float& v : pcm) v *= outGain;
            mbe_floattoshort(pcm, s);
            audio.samples.insert(audio.samples.end(), s, s + 160);
        }
        wav::write(out, audio);
        std::fprintf(stderr, "%ld frames, %ld corrected bit errors, wrote %s\n", frames, totalErrors, out.c_str());
        return 0;
    }
    if (cmd != "decode" || in.empty() || out.empty() || (rate != "dstar" && rate != "ambe2450")) { usage(); return 2; }
    const bool dstar = rate == "dstar";
    mbe_layout layout = dstar ? MBE_LAYOUT_DSTAR : MBE_LAYOUT_DMR;
    if (layoutName == "dstar") layout = MBE_LAYOUT_DSTAR;
    else if (layoutName == "dmr") layout = MBE_LAYOUT_DMR;
    else if (layoutName == "p25p2") layout = MBE_LAYOUT_P25P2;
    else if (!layoutName.empty()) { usage(); return 2; }
    if (bitOrder < 0) bitOrder = dstar ? 0 : 1;

    std::ifstream f(in, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    mbe_parms cur, prev, prevEnh;
    mbe_initMbeParms(&cur, &prev, &prevEnh);
    wav::Audio audio;
    audio.sampleRate = 8000;
    long totalErrors = 0, frames = 0;
    for (size_t i = 0; i + 9 <= data.size(); i += 9, ++frames) {
        char bits[72], fr[4][24], d[49];
        mbe_unpackBits(&data[i], 72, bits, bitOrder);
        mbe_ambeFrameFromBits(layout, bits, fr);
        float pcm[160];
        mbe_process_result r;
        int status;
        if (lost.count(frames)) {
            status = mbe_processLostFramef(pcm, &r, &cur, &prev, &prevEnh);
            std::memset(d, 0, sizeof d);
        } else {
            status = dstar ? mbe_processAmbe3600x2400Framef(pcm, &r, fr, d, &cur, &prev, &prevEnh)
                           : mbe_processAmbe3600x2450Framef(pcm, &r, fr, d, &cur, &prev, &prevEnh);
        }
        if (status < 0) { std::fprintf(stderr, "frame %ld: status %d\n", frames, status); std::memset(pcm, 0, sizeof pcm); }
        else totalErrors += r.total_errors;
        if (printErrors) {
            char bar[64];
            mbe_formatProcessResult(bar, sizeof bar, &r);
            std::printf("%6ld c0=%d prot=%d total=%d %s\n", frames, r.c0_errors, r.protected_errors, r.total_errors, bar);
        }
        short s[160];
        for (float& v : pcm) v *= outGain;
        mbe_floattoshort(pcm, s);
        audio.samples.insert(audio.samples.end(), s, s + 160);
    }
    wav::write(out, audio);
    std::fprintf(stderr, "%ld frames, %ld corrected bit errors, wrote %s\n", frames, totalErrors, out.c_str());
    return 0;
}
