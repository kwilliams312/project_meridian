// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — TLS 1.3 client transport (issue #95). Clean-room from the
// OpenSSL public API + server/libs/net as the interop reference, mirroring the
// proven login_transport.cpp; no GPL source consulted (CONTRIBUTING.md).

#include "meridian/clientnet/tls_client.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include "meridian/clientnet/framing.h"

namespace meridian::clientnet {
namespace {

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
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) err = "TCP connect failed to " + host + ":" + port_str;
    return fd;
}

}  // namespace

TlsClientTransport::TlsClientTransport(const std::string& host, std::uint16_t port) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        error_ = "SSL_CTX_new failed";
        return;
    }
    // Enforce TLS 1.3 as both floor and ceiling, matching the server listener
    // (SAD §5.1) and TlsLoginTransport.
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
    // SNI: help a name-based operator front-end pick the right cert. Harmless for an
    // IP literal / default vhost.
    SSL_set_tlsext_host_name(ssl, host.c_str());

    if (SSL_connect(ssl) != 1) {
        error_ = "TLS handshake failed";
        return;
    }
    connected_ = true;
}

TlsClientTransport::~TlsClientTransport() { close(); }

void TlsClientTransport::close() {
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

std::string TlsClientTransport::tls_version() const {
    if (ssl_ == nullptr) return "";
    return std::string(SSL_get_version(static_cast<SSL*>(ssl_)));
}

bool TlsClientTransport::write_all(const std::uint8_t* buf, std::size_t n) {
    SSL* ssl = static_cast<SSL*>(ssl_);
    std::size_t sent = 0;
    while (sent < n) {
        int w = SSL_write(ssl, buf + sent, static_cast<int>(n - sent));
        if (w <= 0) return false;
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

bool TlsClientTransport::read_all(std::uint8_t* buf, std::size_t n) {
    SSL* ssl = static_cast<SSL*>(ssl_);
    std::size_t got = 0;
    while (got < n) {
        int r = SSL_read(ssl, buf + got, static_cast<int>(n - got));
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool TlsClientTransport::send_frame(const Bytes& payload) {
    if (!connected_) return false;
    std::optional<Bytes> frame = frame_message(payload);
    if (!frame) return false;  // oversize
    return write_all(frame->data(), frame->size());
}

std::optional<Bytes> TlsClientTransport::recv_frame() {
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

void TlsClientTransport::set_recv_timeout_ms(unsigned ms) {
    recv_timeout_ms_ = ms;
    if (fd_ < 0) return;
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(ms / 1000u);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000u) * 1000u);
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Read exactly n bytes; report a clean read-timeout on the FIRST byte via
// `timed_out`. Under SO_RCVTIMEO a timed-out SSL_read returns <=0 with
// SSL_ERROR_WANT_READ or SSL_ERROR_SYSCALL + errno EAGAIN/EWOULDBLOCK. Mirrors
// login_transport.cpp read_all_timed.
bool TlsClientTransport::read_all_timed(std::uint8_t* buf, std::size_t n, bool& timed_out) {
    timed_out = false;
    SSL* ssl = static_cast<SSL*>(ssl_);
    std::size_t got = 0;
    while (got < n) {
        int r = SSL_read(ssl, buf + got, static_cast<int>(n - got));
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            continue;
        }
        const int e = SSL_get_error(ssl, r);
        const bool would_block =
            (e == SSL_ERROR_WANT_READ) ||
            (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK));
        if (would_block && got == 0) {
            timed_out = true;
            return false;
        }
        return false;  // peer closed, hard error, or timeout mid-frame (partial)
    }
    return true;
}

std::optional<Bytes> TlsClientTransport::recv_frame_nb(bool& would_block) {
    would_block = false;
    if (!connected_) return std::nullopt;
    std::uint8_t lenbuf[kLengthPrefixBytes];
    bool timed_out = false;
    if (!read_all_timed(lenbuf, kLengthPrefixBytes, timed_out)) {
        would_block = timed_out;
        return std::nullopt;
    }
    const Bytes lb(lenbuf, lenbuf + kLengthPrefixBytes);
    const std::uint32_t len = *read_length_prefix(lb);
    if (!length_is_valid(len)) return std::nullopt;
    Bytes payload(len);
    if (len > 0) {
        bool body_timed_out = false;
        if (!read_all_timed(payload.data(), len, body_timed_out)) return std::nullopt;
    }
    return payload;
}

}  // namespace meridian::clientnet
