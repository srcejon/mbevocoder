// SPDX-License-Identifier: GPL-3.0-or-later
// Host driver for the DVSI AMBE-3000R vocoder chip in UART packet mode
// (as found in ThumbDV / DV3000U dongles). Packet formats from the
// AMBE-3000R Users Manual v1.4, section 6.
#pragma once

#include "serial_port.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ambe3000 {

constexpr uint8_t START_BYTE = 0x61;
constexpr uint8_t TYPE_CONTROL = 0x00;
constexpr uint8_t TYPE_CHANNEL = 0x01;
constexpr uint8_t TYPE_SPEECH  = 0x02;

// Control field identifiers (Table 106 / section 6).
enum Field : uint8_t {
    PKT_CHANNEL0    = 0x40,
    PKT_ECMODE      = 0x05,
    PKT_DCMODE      = 0x06,
    PKT_COMPAND     = 0x32,
    PKT_RATET       = 0x09,
    PKT_RATEP       = 0x0A,
    PKT_INIT        = 0x0B,
    PKT_LOWPOWER    = 0x10,
    PKT_CHANFMT     = 0x15,
    PKT_SPCHFMT     = 0x16,
    PKT_PRODID      = 0x30,
    PKT_VERSTRING   = 0x31,
    PKT_RESET       = 0x33,
    PKT_READY       = 0x39,
    PKT_PARITYMODE  = 0x3F,
    PKT_GAIN        = 0x4B,
    // data packet fields
    PKT_SPEECHD     = 0x00,
    PKT_CHAND       = 0x01,
    PKT_CMODE       = 0x02,
    PKT_SAMPLES     = 0x03,
    PKT_TONE        = 0x08,
    PKT_CHAND4      = 0x17,
};

enum class Rate {
    DStar,      // AMBE 3600x2400 via PKT_RATEP (no rate index exists)
    Ambe2450,   // AMBE+2 3600x2450, PKT_RATET 33 (DMR, P25 Phase 2, YSF DN, NXDN)
    Raw2450,    // AMBE+2 2450x0, PKT_RATET 34 (49 voice bits, no FEC)
};

const char* rateName(Rate r);
bool parseRate(const std::string& s, Rate& out);
unsigned rateBits(Rate r);              // channel bits per frame: 72 or 49
unsigned rateBytes(Rate r);             // ceil(bits/8)

constexpr unsigned SAMPLES_PER_FRAME = 160;
using Pcm = std::array<int16_t, SAMPLES_PER_FRAME>;

struct Packet {
    uint8_t type = 0;
    std::vector<uint8_t> payload;   // everything after the 4-byte header
};

class Device {
public:
    explicit Device(SerialPort& port) : m_port(port) {}

    // Low level
    void send(uint8_t type, const std::vector<uint8_t>& payload);
    Packet receive(unsigned timeoutMs = 2000);
    Packet control(const std::vector<uint8_t>& fields, unsigned timeoutMs = 2000);

    // Identification
    std::string productId();
    std::string versionString();

    // Reset and wait for PKT_READY. Returns false if no READY packet arrived.
    bool reset(unsigned timeoutMs = 3000);

    // Configure for clean reference operation: rate, PKT_INIT both directions,
    // ECMODE/DCMODE 0 (no tone detect, DTX, NS, companding), gain 0/0, linear PCM.
    void configure(Rate rate);
    // Override the encoder (PKT_ECMODE) or decoder (PKT_DCMODE) option word after configure().
    void setMode(bool encoder, uint16_t mode);
    // Add a PKT_CMODE field with this value to every speech packet sent to the encoder (-1 = none).
    void setSpeechCmode(int cmode);
    // Per-frame DCMODE_IN for the next decode(): entry i is sent as PKT_CMODE with channel
    // packet i (-1 = no field). Bit 2 LOST_FRAME makes the decoder repeat instead of decoding;
    // the chip keeps DCMODE_IN until it is changed, so send 0 on the frames after a lost one.
    void setFrameCmodes(std::vector<int> cmodes) { m_frameCmode = std::move(cmodes); }

    // One frame each direction, synchronous.
    std::vector<uint8_t> encodeFrame(const Pcm& pcm);          // returns rateBytes(rate) bytes
    Pcm decodeFrame(const uint8_t* bits, unsigned nbits);

    // Pipelined batch processing (keeps `depth` requests in flight; AMBE-3000R FIFOs hold 2).
    // Output has exactly one entry per input.
    std::vector<std::vector<uint8_t>> encode(const std::vector<Pcm>& frames, unsigned depth = 2,
                                             const std::function<void(size_t)>& progress = {});
    std::vector<Pcm> decode(const std::vector<std::vector<uint8_t>>& frames, unsigned nbits, unsigned depth = 2,
                            const std::function<void(size_t)>& progress = {});

    Rate rate() const { return m_rate; }
    // The PKT_CMODE (ECMODE_OUT) word of each channel packet from the last encode(), -1 where
    // the AMBE-3000R sent none. Bit 1 VOICE_ACTIVE, bit 15 TONE_FRAME.
    const std::vector<int>& cmodeOut() const { return m_cmodeOut; }

private:
    static std::vector<uint8_t> parseChannelResponse(const Packet& p, unsigned expectedBits, int* cmodeOut);
    static Pcm parseSpeechResponse(const Packet& p);
    void expectAck(const Packet& p, uint8_t field, const char* what);

    SerialPort& m_port;
    Rate m_rate = Rate::Ambe2450;
    std::vector<int> m_cmodeOut;
    std::vector<int> m_frameCmode;
};

} // namespace ambe3000
