// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal blocking serial port with read timeout. Win32 and POSIX (Linux, macOS).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // Throws std::runtime_error on failure. 8N1, no flow control.
    void open(const std::string& port, unsigned baud);
    void close();
    bool isOpen() const;

    void write(const uint8_t* data, size_t len);
    // Reads up to len bytes, returning as soon as any data arrives or timeoutMs elapses.
    // Returns number of bytes read (0 on timeout).
    size_t read(uint8_t* data, size_t len, unsigned timeoutMs);
    // Reads exactly len bytes or throws on timeout.
    void readExact(uint8_t* data, size_t len, unsigned timeoutMs);
    void discardInput();

private:
#ifdef _WIN32
    void* m_handle = nullptr;   // HANDLE
#else
    int m_fd = -1;
#endif
};
