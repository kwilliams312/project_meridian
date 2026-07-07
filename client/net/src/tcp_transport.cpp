// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — plain-TCP frame transport (issue #95). Factored from the
// socket + framing layer of this repo's proven login_transport.cpp (minus TLS); no
// GPL source consulted (CONTRIBUTING.md).

#include "meridian/clientnet/tcp_transport.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include "meridian/clientnet/framing.h"

namespace meridian::clientnet {
namespace {

// Resolve host:port and connect a TCP socket. Returns the connected fd, or -1 with
// `err` set. Tries each resolved address in order (v4/v6). Identical strategy to
// login_transport.cpp tcp_connect.
int tcp_connect(const std::string& host, std::uint16_t port, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port_str = std::to_string(port);
    addrinfo* res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || res == nullptr) {
        err = std::string("getaddrinfo failed: ") + gai_strerror(gai);
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;  // connected
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) err = "TCP connect failed to " + host + ":" + port_str;
    return fd;
}

}  // namespace

TcpTransport::TcpTransport(const std::string& host, std::uint16_t port) {
    fd_ = tcp_connect(host, port, error_);
    connected_ = (fd_ >= 0);
}

TcpTransport::~TcpTransport() { close(); }

void TcpTransport::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

bool TcpTransport::write_all(const std::uint8_t* buf, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::send(fd_, buf + sent, n - sent, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

bool TcpTransport::read_all(std::uint8_t* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd_, buf + got, n - got, 0);
        if (r == 0) return false;  // peer closed
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool TcpTransport::send_frame(const Bytes& payload) {
    if (!connected_) return false;
    std::optional<Bytes> frame = frame_message(payload);
    if (!frame) return false;  // oversize
    return write_all(frame->data(), frame->size());
}

std::optional<Bytes> TcpTransport::recv_frame() {
    if (!connected_) return std::nullopt;
    std::uint8_t lenbuf[kLengthPrefixBytes];
    if (!read_all(lenbuf, kLengthPrefixBytes)) return std::nullopt;
    const Bytes lb(lenbuf, lenbuf + kLengthPrefixBytes);
    const std::uint32_t len = *read_length_prefix(lb);
    if (!length_is_valid(len)) return std::nullopt;  // reject BEFORE allocating
    Bytes payload(len);
    if (len > 0 && !read_all(payload.data(), len)) return std::nullopt;
    return payload;
}

void TcpTransport::set_recv_timeout_ms(unsigned ms) {
    recv_timeout_ms_ = ms;
    if (fd_ < 0) return;
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(ms / 1000u);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000u) * 1000u);
    // ms == 0 sets an all-zero timeval — POSIX "no timeout" (blocking restore).
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Read exactly n bytes, reporting a clean read-timeout on the FIRST byte (nothing
// consumed) via `timed_out`. A timeout after ≥1 byte is a hard failure (partial
// frame). Mirrors login_transport.cpp read_all_timed.
bool TcpTransport::read_all_timed(std::uint8_t* buf, std::size_t n, bool& timed_out) {
    timed_out = false;
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd_, buf + got, n - got, 0);
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            continue;
        }
        if (r == 0) return false;  // peer closed
        if (errno == EINTR) continue;
        const bool would_block = (errno == EAGAIN || errno == EWOULDBLOCK);
        if (would_block && got == 0) {
            timed_out = true;  // no byte consumed — a clean "nothing pending yet"
            return false;
        }
        return false;  // hard error, or timeout mid-frame (partial)
    }
    return true;
}

std::optional<Bytes> TcpTransport::recv_frame_nb(bool& would_block) {
    would_block = false;
    if (!connected_) return std::nullopt;
    std::uint8_t lenbuf[kLengthPrefixBytes];
    bool timed_out = false;
    if (!read_all_timed(lenbuf, kLengthPrefixBytes, timed_out)) {
        would_block = timed_out;  // true only if the length read timed out with 0 bytes
        return std::nullopt;
    }
    const Bytes lb(lenbuf, lenbuf + kLengthPrefixBytes);
    const std::uint32_t len = *read_length_prefix(lb);
    if (!length_is_valid(len)) return std::nullopt;
    Bytes payload(len);
    if (len > 0) {
        bool body_timed_out = false;  // a mid-frame timeout is NOT would_block (partial)
        if (!read_all_timed(payload.data(), len, body_timed_out)) return std::nullopt;
    }
    return payload;
}

}  // namespace meridian::clientnet
