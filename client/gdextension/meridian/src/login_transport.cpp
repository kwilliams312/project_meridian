// SPDX-License-Identifier: Apache-2.0
//
// meridian client TLS 1.3 login transport implementation (issue #99). Clean-room
// from the OpenSSL public API + server/libs/net as the interop reference; no GPL
// source consulted (CONTRIBUTING.md).

#include "login_transport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace meridian::login {

namespace {
// IF-1 framing bound (server SAD §5.1 / meridian-net kMaxFrameBytes): the u32-LE
// length prefix excludes itself and MUST NOT exceed 8 KiB.
constexpr std::uint32_t kMaxFrameBytes = 8u * 1024u;

// Resolve host:port and connect a TCP socket. Returns the connected fd, or -1
// with `err` set. Tries each resolved address in order (v4/v6).
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

TlsLoginTransport::TlsLoginTransport(const std::string& host, std::uint16_t port) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        error_ = "SSL_CTX_new failed";
        return;
    }
    // Enforce TLS 1.3 as the minimum, matching the server's listener (SAD §5.1).
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    // M0: no cert-chain verification (SSL_VERIFY_NONE default) — see header note.
    ctx_ = ctx;

    fd_ = tcp_connect(host, port, error_);
    if (fd_ < 0) return;

    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        error_ = "SSL_new failed";
        return;
    }
    ssl_ = ssl;
    SSL_set_fd(ssl, fd_);
    // SNI: help a name-based operator front-end pick the right cert. Harmless for
    // an IP literal / default vhost.
    SSL_set_tlsext_host_name(ssl, host.c_str());

    if (SSL_connect(ssl) != 1) {
        error_ = "TLS handshake failed";
        return;
    }
    connected_ = true;
}

TlsLoginTransport::~TlsLoginTransport() { close(); }

void TlsLoginTransport::close() {
    if (ssl_ != nullptr) {
        SSL* ssl = static_cast<SSL*>(ssl_);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (ctx_ != nullptr) {
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
        ctx_ = nullptr;
    }
    connected_ = false;
}

std::string TlsLoginTransport::tls_version() const {
    if (ssl_ == nullptr) return "";
    return std::string(SSL_get_version(static_cast<SSL*>(ssl_)));
}

bool TlsLoginTransport::write_all(const std::uint8_t* buf, std::size_t n) {
    SSL* ssl = static_cast<SSL*>(ssl_);
    std::size_t sent = 0;
    while (sent < n) {
        int w = SSL_write(ssl, buf + sent, static_cast<int>(n - sent));
        if (w <= 0) return false;
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

bool TlsLoginTransport::read_all(std::uint8_t* buf, std::size_t n) {
    SSL* ssl = static_cast<SSL*>(ssl_);
    std::size_t got = 0;
    while (got < n) {
        int r = SSL_read(ssl, buf + got, static_cast<int>(n - got));
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool TlsLoginTransport::send_frame(const Bytes& payload) {
    if (!connected_) return false;
    if (payload.size() > kMaxFrameBytes) return false;
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    Bytes frame{static_cast<std::uint8_t>(len & 0xFF),
                static_cast<std::uint8_t>((len >> 8) & 0xFF),
                static_cast<std::uint8_t>((len >> 16) & 0xFF),
                static_cast<std::uint8_t>((len >> 24) & 0xFF)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return write_all(frame.data(), frame.size());
}

std::optional<Bytes> TlsLoginTransport::recv_frame() {
    if (!connected_) return std::nullopt;
    std::uint8_t lenbuf[4];
    if (!read_all(lenbuf, 4)) return std::nullopt;
    const std::uint32_t len = static_cast<std::uint32_t>(lenbuf[0]) |
                              (static_cast<std::uint32_t>(lenbuf[1]) << 8) |
                              (static_cast<std::uint32_t>(lenbuf[2]) << 16) |
                              (static_cast<std::uint32_t>(lenbuf[3]) << 24);
    // Reject an oversize prefix BEFORE allocating (untrusted length — a broken /
    // hostile server must not induce a huge alloc; matches meridian-net).
    if (len > kMaxFrameBytes) return std::nullopt;
    Bytes payload(len);
    if (len > 0 && !read_all(payload.data(), len)) return std::nullopt;
    return payload;
}

}  // namespace meridian::login
