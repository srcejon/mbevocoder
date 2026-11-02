// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_port.hpp"

#include <stdexcept>
#include <chrono>

#ifdef _WIN32
#include <windows.h>

SerialPort::~SerialPort() { close(); }

void SerialPort::open(const std::string& port, unsigned baud)
{
    close();
    std::string path = port;
    if (path.rfind("\\\\.\\", 0) != 0) path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open " + port + " (error " + std::to_string(GetLastError()) + ")");

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); throw std::runtime_error("GetCommState failed"); }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); throw std::runtime_error("SetCommState failed (baud " + std::to_string(baud) + ")"); }
    SetupComm(h, 1 << 16, 1 << 16);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    m_handle = h;
}

void SerialPort::close()
{
    if (m_handle) { CloseHandle(static_cast<HANDLE>(m_handle)); m_handle = nullptr; }
}

bool SerialPort::isOpen() const { return m_handle != nullptr; }

void SerialPort::write(const uint8_t* data, size_t len)
{
    DWORD written = 0;
    while (len > 0) {
        if (!WriteFile(static_cast<HANDLE>(m_handle), data, static_cast<DWORD>(len), &written, nullptr))
            throw std::runtime_error("serial write failed");
        data += written; len -= written;
    }
}

size_t SerialPort::read(uint8_t* data, size_t len, unsigned timeoutMs)
{
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;          // return as soon as some bytes are available...
    to.ReadTotalTimeoutMultiplier = MAXDWORD;   // ...but wait up to timeoutMs for the first byte
    to.ReadTotalTimeoutConstant = timeoutMs;
    SetCommTimeouts(static_cast<HANDLE>(m_handle), &to);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(m_handle), data, static_cast<DWORD>(len), &got, nullptr))
        throw std::runtime_error("serial read failed");
    return got;
}

void SerialPort::discardInput()
{
    PurgeComm(static_cast<HANDLE>(m_handle), PURGE_RXCLEAR);
}

#else  // POSIX

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#endif

SerialPort::~SerialPort() { close(); }

void SerialPort::open(const std::string& port, unsigned baud)
{
    close();
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("cannot open " + port + ": " + std::strerror(errno));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) { ::close(fd); throw std::runtime_error("tcgetattr failed"); }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    bool speedSet = false;
#ifndef __APPLE__
    speed_t sp = 0;
    switch (baud) {
        case 57600: sp = B57600; break;
        case 115200: sp = B115200; break;
        case 230400: sp = B230400; break;
#ifdef B460800
        case 460800: sp = B460800; break;
#endif
        default: break;
    }
    if (sp) { cfsetispeed(&tio, sp); cfsetospeed(&tio, sp); speedSet = true; }
#endif
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { ::close(fd); throw std::runtime_error("tcsetattr failed"); }
#ifdef __APPLE__
    speed_t custom = baud;
    if (ioctl(fd, IOSSIOSPEED, &custom) == 0) speedSet = true;
#endif
    if (!speedSet) { ::close(fd); throw std::runtime_error("unsupported baud rate " + std::to_string(baud)); }
    tcflush(fd, TCIOFLUSH);
    m_fd = fd;
}

void SerialPort::close()
{
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
}

bool SerialPort::isOpen() const { return m_fd >= 0; }

void SerialPort::write(const uint8_t* data, size_t len)
{
    while (len > 0) {
        ssize_t n = ::write(m_fd, data, len);
        if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("serial write failed"); }
        data += n; len -= static_cast<size_t>(n);
    }
}

size_t SerialPort::read(uint8_t* data, size_t len, unsigned timeoutMs)
{
    pollfd p{}; p.fd = m_fd; p.events = POLLIN;
    int r = ::poll(&p, 1, static_cast<int>(timeoutMs));
    if (r <= 0) return 0;
    ssize_t n = ::read(m_fd, data, len);
    if (n < 0) throw std::runtime_error("serial read failed");
    return static_cast<size_t>(n);
}

void SerialPort::discardInput()
{
    tcflush(m_fd, TCIFLUSH);
}

#endif

void SerialPort::readExact(uint8_t* data, size_t len, unsigned timeoutMs)
{
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    while (len > 0) {
        auto now = clock::now();
        if (now >= deadline) throw std::runtime_error("serial read timeout");
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        size_t got = read(data, len, static_cast<unsigned>(remain));
        data += got; len -= got;
    }
}
