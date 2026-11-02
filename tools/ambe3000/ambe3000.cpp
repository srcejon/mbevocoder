// SPDX-License-Identifier: GPL-3.0-or-later
#include "ambe3000.hpp"

#include <stdexcept>
#include <cstring>

namespace ambe3000 {

const char* rateName(Rate r)
{
    switch (r) {
        case Rate::DStar: return "dstar";
        case Rate::Ambe2450: return "ambe2450";
        case Rate::Raw2450: return "raw2450";
    }
    return "?";
}

bool parseRate(const std::string& s, Rate& out)
{
    if (s == "dstar" || s == "3600x2400") { out = Rate::DStar; return true; }
    if (s == "ambe2450" || s == "dmr" || s == "3600x2450") { out = Rate::Ambe2450; return true; }
    if (s == "raw2450" || s == "2450") { out = Rate::Raw2450; return true; }
    return false;
}

unsigned rateBits(Rate r) { return r == Rate::Raw2450 ? 49u : 72u; }
unsigned rateBytes(Rate r) { return (rateBits(r) + 7) / 8; }

void Device::send(uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> buf;
    buf.reserve(payload.size() + 4);
    buf.push_back(START_BYTE);
    buf.push_back(static_cast<uint8_t>(payload.size() >> 8));
    buf.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    buf.push_back(type);
    buf.insert(buf.end(), payload.begin(), payload.end());
    m_port.write(buf.data(), buf.size());
}

Packet Device::receive(unsigned timeoutMs)
{
    // Hunt for the start byte, then read the header and payload.
    uint8_t b = 0;
    for (;;) {
        m_port.readExact(&b, 1, timeoutMs);
        if (b == START_BYTE) break;
    }
    uint8_t hdr[3];
    m_port.readExact(hdr, 3, timeoutMs);
    Packet p;
    p.type = hdr[2];
    size_t len = (static_cast<size_t>(hdr[0]) << 8) | hdr[1];
    if (len > 4096) throw std::runtime_error("implausible packet length");
    p.payload.resize(len);
    if (len) m_port.readExact(p.payload.data(), len, timeoutMs);
    return p;
}

Packet Device::control(const std::vector<uint8_t>& fields, unsigned timeoutMs)
{
    send(TYPE_CONTROL, fields);
    Packet p = receive(timeoutMs);
    if (p.type != TYPE_CONTROL) throw std::runtime_error("expected control response");
    return p;
}

// The bytes up to the first NUL, or all of them (strnlen is not in strict C++17).
static std::string boundedString(const uint8_t* data, size_t n)
{
    size_t len = 0;
    while (len < n && data[len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(data), len);
}

std::string Device::productId()
{
    Packet p = control({PKT_PRODID});
    if (p.payload.empty() || p.payload[0] != PKT_PRODID) throw std::runtime_error("bad PRODID response");
    return boundedString(p.payload.data() + 1, p.payload.size() - 1);
}

std::string Device::versionString()
{
    Packet p = control({PKT_VERSTRING});
    if (p.payload.empty() || p.payload[0] != PKT_VERSTRING) throw std::runtime_error("bad VERSTRING response");
    return boundedString(p.payload.data() + 1, p.payload.size() - 1);
}

bool Device::reset(unsigned timeoutMs)
{
    m_port.discardInput();
    send(TYPE_CONTROL, {PKT_RESET});
    try {
        Packet p = receive(timeoutMs);
        return p.type == TYPE_CONTROL && !p.payload.empty() && p.payload[0] == PKT_READY;
    } catch (const std::exception&) {
        return false;
    }
}

void Device::expectAck(const Packet& p, uint8_t field, const char* what)
{
    if (p.payload.size() < 2 || p.payload[0] != field || p.payload[1] != 0x00)
        throw std::runtime_error(std::string("chip rejected ") + what);
}

void Device::configure(Rate rate)
{
    m_rate = rate;
    switch (rate) {
        case Rate::DStar:
            // Table 116, "interoperable with DSTAR": 3600 bps total, 2400 speech, 1200 FEC.
            expectAck(control({PKT_RATEP, 0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48}),
                      PKT_RATEP, "PKT_RATEP (D-STAR)");
            break;
        case Rate::Ambe2450:
            expectAck(control({PKT_RATET, 33}), PKT_RATET, "PKT_RATET 33");
            break;
        case Rate::Raw2450:
            expectAck(control({PKT_RATET, 34}), PKT_RATET, "PKT_RATET 34");
            break;
    }
    expectAck(control({PKT_INIT, 0x03}), PKT_INIT, "PKT_INIT");
    // PKT_INIT re-enables tone detection; clear every encoder/decoder option afterwards.
    expectAck(control({PKT_ECMODE, 0x00, 0x00}), PKT_ECMODE, "PKT_ECMODE");
    expectAck(control({PKT_DCMODE, 0x00, 0x00}), PKT_DCMODE, "PKT_DCMODE");
    expectAck(control({PKT_GAIN, 0x00, 0x00}), PKT_GAIN, "PKT_GAIN");
    expectAck(control({PKT_COMPAND, 0x00}), PKT_COMPAND, "PKT_COMPAND");
    // Plain output packets: no ecmode/dcmode/samples fields.
    expectAck(control({PKT_CHANFMT, 0x00, 0x00}), PKT_CHANFMT, "PKT_CHANFMT");
    expectAck(control({PKT_SPCHFMT, 0x00, 0x00}), PKT_SPCHFMT, "PKT_SPCHFMT");
}

void Device::setMode(bool encoder, uint16_t mode)
{
    uint8_t field = encoder ? PKT_ECMODE : PKT_DCMODE;
    expectAck(control({field, static_cast<uint8_t>(mode >> 8), static_cast<uint8_t>(mode & 0xff)}), field,
              encoder ? "PKT_ECMODE" : "PKT_DCMODE");
}

static int g_cmode = -1;   // per-packet PKT_CMODE field for speech packets, -1 = none

void Device::setSpeechCmode(int cmode) { g_cmode = cmode; }

static std::vector<uint8_t> speechPayload(const Pcm& pcm)
{
    std::vector<uint8_t> f;
    f.reserve(5 + 2 * SAMPLES_PER_FRAME);
    if (g_cmode >= 0) {
        f.push_back(PKT_CMODE);
        f.push_back(static_cast<uint8_t>((g_cmode >> 8) & 0xff));
        f.push_back(static_cast<uint8_t>(g_cmode & 0xff));
    }
    f.push_back(PKT_SPEECHD);
    f.push_back(static_cast<uint8_t>(SAMPLES_PER_FRAME));
    for (int16_t s : pcm) {
        f.push_back(static_cast<uint8_t>((static_cast<uint16_t>(s) >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>(static_cast<uint16_t>(s) & 0xFF));
    }
    return f;
}

static std::vector<uint8_t> channelPayload(const uint8_t* bits, unsigned nbits, int cmode = -1)
{
    std::vector<uint8_t> f;
    if (cmode >= 0) {   // DCMODE_IN for this frame (bit 2 LOST_FRAME: repeat instead of decoding the bits)
        f.push_back(PKT_CMODE);
        f.push_back(static_cast<uint8_t>((cmode >> 8) & 0xff));
        f.push_back(static_cast<uint8_t>(cmode & 0xff));
    }
    f.push_back(PKT_CHAND);
    f.push_back(static_cast<uint8_t>(nbits));
    f.insert(f.end(), bits, bits + (nbits + 7) / 8);
    return f;
}

std::vector<uint8_t> Device::parseChannelResponse(const Packet& p, unsigned expectedBits, int* cmodeOut)
{
    if (p.type != TYPE_CHANNEL) throw std::runtime_error("expected channel packet");
    size_t i = 0;
    const auto& d = p.payload;
    if (cmodeOut) *cmodeOut = -1;
    std::vector<uint8_t> bits;
    bool have = false;
    while (i < d.size()) {
        uint8_t id = d[i++];
        switch (id) {
            case PKT_CHAND: {
                if (i >= d.size()) throw std::runtime_error("truncated CHAND");
                unsigned nbits = d[i++];
                unsigned nbytes = (nbits + 7) / 8;
                if (i + nbytes > d.size()) throw std::runtime_error("truncated CHAND data");
                if (nbits != expectedBits) throw std::runtime_error("unexpected channel bit count " + std::to_string(nbits));
                bits.assign(d.begin() + static_cast<long>(i), d.begin() + static_cast<long>(i + nbytes));
                have = true;
                i += nbytes;
                break;
            }
            case PKT_CMODE:   // ECMODE_OUT: VOICE_ACTIVE (bit 1), TONE_FRAME (bit 15), ...
                if (i + 2 > d.size()) throw std::runtime_error("truncated CMODE");
                if (cmodeOut) *cmodeOut = (d[i] << 8) | d[i + 1];
                i += 2;
                break;
            case PKT_SAMPLES: i += 1; break;
            case PKT_TONE: i += 2; break;
            case PKT_CHANNEL0: break;
            default: throw std::runtime_error("unknown field in channel packet: " + std::to_string(id));
        }
    }
    if (!have) throw std::runtime_error("channel packet without CHAND field");
    return bits;
}

Pcm Device::parseSpeechResponse(const Packet& p)
{
    if (p.type != TYPE_SPEECH) throw std::runtime_error("expected speech packet");
    size_t i = 0;
    const auto& d = p.payload;
    while (i < d.size()) {
        uint8_t id = d[i++];
        switch (id) {
            case PKT_SPEECHD: {
                if (i >= d.size()) throw std::runtime_error("truncated SPEECHD");
                unsigned n = d[i++];
                if (i + 2 * n > d.size()) throw std::runtime_error("truncated SPEECHD data");
                Pcm pcm{};
                // The AMBE-3000R may return 156..164 samples when clock-slip compensation is on;
                // we run without it, so anything other than 160 is unexpected but tolerated.
                for (unsigned k = 0; k < n && k < SAMPLES_PER_FRAME; ++k)
                    pcm[k] = static_cast<int16_t>((d[i + 2 * k] << 8) | d[i + 2 * k + 1]);
                return pcm;
            }
            case PKT_CMODE: i += 2; break;
            case PKT_SAMPLES: i += 1; break;
            case PKT_TONE: i += 2; break;
            case PKT_CHANNEL0: break;
            default: throw std::runtime_error("unknown field in speech packet: " + std::to_string(id));
        }
    }
    throw std::runtime_error("speech packet without SPEECHD field");
}

std::vector<uint8_t> Device::encodeFrame(const Pcm& pcm)
{
    send(TYPE_SPEECH, speechPayload(pcm));
    int cmode;
    auto bits = parseChannelResponse(receive(), rateBits(m_rate), &cmode);
    m_cmodeOut.assign(1, cmode);
    return bits;
}

Pcm Device::decodeFrame(const uint8_t* bits, unsigned nbits)
{
    send(TYPE_CHANNEL, channelPayload(bits, nbits));
    return parseSpeechResponse(receive());
}

std::vector<std::vector<uint8_t>> Device::encode(const std::vector<Pcm>& frames, unsigned depth,
                                                 const std::function<void(size_t)>& progress)
{
    std::vector<std::vector<uint8_t>> out;
    out.reserve(frames.size());
    m_cmodeOut.clear();
    m_cmodeOut.reserve(frames.size());
    if (depth < 1) depth = 1;
    size_t sent = 0, received = 0;
    while (received < frames.size()) {
        while (sent < frames.size() && sent - received < depth) {
            send(TYPE_SPEECH, speechPayload(frames[sent]));
            ++sent;
        }
        int cmode;
        out.push_back(parseChannelResponse(receive(), rateBits(m_rate), &cmode));
        m_cmodeOut.push_back(cmode);
        ++received;
        if (progress && (received % 250 == 0 || received == frames.size())) progress(received);
    }
    return out;
}

std::vector<Pcm> Device::decode(const std::vector<std::vector<uint8_t>>& frames, unsigned nbits, unsigned depth,
                                const std::function<void(size_t)>& progress)
{
    std::vector<Pcm> out;
    out.reserve(frames.size());
    if (depth < 1) depth = 1;
    size_t sent = 0, received = 0;
    while (received < frames.size()) {
        while (sent < frames.size() && sent - received < depth) {
            const int cmode = sent < m_frameCmode.size() ? m_frameCmode[sent] : -1;
            send(TYPE_CHANNEL, channelPayload(frames[sent].data(), nbits, cmode));
            ++sent;
        }
        out.push_back(parseSpeechResponse(receive()));
        ++received;
        if (progress && (received % 250 == 0 || received == frames.size())) progress(received);
    }
    return out;
}

} // namespace ambe3000
