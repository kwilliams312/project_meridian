// SPDX-License-Identifier: Apache-2.0
//
// meridian-net TLS 1.3 listener implementation.
//
// CLEAN-ROOM: implemented from the server SAD (§5.1 IF-1 transport + framing,
// §2.1 acceptor model) and the OpenSSL public API docs only. No GPL source
// (CMaNGOS / TrinityCore or otherwise) was consulted. See CONTRIBUTING.md.
//
// Design notes:
//   - Transport is plain BSD sockets (socket/bind/listen/accept) with OpenSSL
//     layered on the accepted fd. No Asio/Boost — the "small IO pool" of SAD §2.1
//     is the caller's to build (acceptor thread calls accept(), workers drive
//     Sessions).
//   - TLS 1.3 is ENFORCED as the floor: SSL_CTX_set_min_proto_version(
//     TLS1_3_VERSION). A ClientHello that can only do <= 1.2 fails the handshake.
//   - IF-1 framing (§5.1): every frame is `u32 LE length | payload`, length
//     excludes itself and is capped at kMaxFrameBytes (8 KiB). The cap is checked
//     against the wire prefix BEFORE allocating the payload buffer.

#include "meridian/net/tls_listener.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace meridian::net {
namespace {

// OpenSSL library init is process-global and idempotent; guard it so multiple
// listeners in one process (or a test creating several) init exactly once.
void ensure_openssl_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        // With OpenSSL 1.1+/3.x explicit init is optional (auto-init on first
        // use), but calling it makes ordering explicit and is harmless.
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                             OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         nullptr);
        // A write to a peer that has closed its half of the connection raises
        // SIGPIPE, whose default disposition kills the process. A network server
        // must never let a misbehaving/departed peer take it down: ignore it and
        // rely on SSL_write/SSL_read returning an error instead. (macOS lacks a
        // reliable MSG_NOSIGNAL on the SSL path, so we suppress the signal.)
        std::signal(SIGPIPE, SIG_IGN);
    });
}

// Drain OpenSSL's error queue into a single readable string for TlsError.
std::string openssl_errors() {
    std::string out;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out.empty() ? "unknown OpenSSL error" : out;
}

[[noreturn]] void throw_tls(const std::string& ctx) {
    throw TlsError(ctx + ": " + openssl_errors());
}

[[noreturn]] void throw_errno(const std::string& ctx) {
    throw TlsError(ctx + ": " + std::string(std::strerror(errno)));
}

// Build the shared server SSL_CTX: TLS 1.3 minimum, cert + key from PEM files.
SSL_CTX* make_server_ctx(const ListenConfig& cfg) {
    ensure_openssl_init();

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) throw_tls("SSL_CTX_new");

    // ENFORCE TLS 1.3 as the floor. Also pin the ceiling to 1.3 so nothing above
    // (should a future protocol appear) is silently accepted without review.
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1) {
        SSL_CTX_free(ctx);
        throw_tls("SSL_CTX_set_min_proto_version(TLS1_3)");
    }
    if (SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1) {
        SSL_CTX_free(ctx);
        throw_tls("SSL_CTX_set_max_proto_version(TLS1_3)");
    }

    if (SSL_CTX_use_certificate_chain_file(ctx, cfg.cert_path.c_str()) != 1) {
        std::string err = openssl_errors();
        SSL_CTX_free(ctx);
        throw TlsError("load certificate '" + cfg.cert_path + "': " + err);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg.key_path.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
        std::string err = openssl_errors();
        SSL_CTX_free(ctx);
        throw TlsError("load private key '" + cfg.key_path + "': " + err);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        std::string err = openssl_errors();
        SSL_CTX_free(ctx);
        throw TlsError("certificate/key mismatch: " + err);
    }

    return ctx;
}

// Format a connected peer's address as "ip:port" (best effort).
std::string format_peer(const sockaddr_storage& ss) {
    char host[INET6_ADDRSTRLEN] = {0};
    std::uint16_t port = 0;
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        port = ntohs(a->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

}  // namespace

// ---- Session ----------------------------------------------------------------

struct Session::Impl {
    int fd = -1;
    SSL* ssl = nullptr;
    std::string peer;

    ~Impl() {
        if (ssl) {
            // Best-effort orderly close; ignore result (peer may already be gone).
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

const std::string& Session::peer() const { return impl_->peer; }

std::string Session::tls_version() const {
    if (!impl_ || !impl_->ssl) return {};
    const char* v = SSL_get_version(impl_->ssl);
    return v ? std::string(v) : std::string{};
}

// Read exactly n bytes from the TLS stream into buf. Loops over SSL_read until
// satisfied. at_frame_start distinguishes a clean EOF (peer done) from a
// truncated frame (peer misbehaved) for the first byte of a frame.
static void ssl_read_exact(SSL* ssl, std::uint8_t* buf, std::size_t n,
                           bool at_frame_start) {
    std::size_t got = 0;
    while (got < n) {
        int r = SSL_read(ssl, buf + got, static_cast<int>(n - got));
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            at_frame_start = false;  // any progress means we're mid-frame now
            continue;
        }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) {
            // Peer sent close_notify. Clean only if we hadn't started a frame.
            if (at_frame_start && got == 0) throw ConnectionClosed();
            throw TlsError("connection closed mid-frame");
        }
        if (err == SSL_ERROR_SYSCALL && got == 0 && at_frame_start) {
            // Peer dropped the TCP connection without close_notify (common for
            // simple clients). Treat a clean EOF at a frame boundary as closed.
            if (ERR_peek_error() == 0) throw ConnectionClosed();
            throw_tls("SSL_read");
        }
        throw_tls("SSL_read");
    }
}

Bytes Session::read_frame() {
    if (!impl_ || !impl_->ssl) throw TlsError("read_frame on closed session");

    // 1) Read the 4-byte u32 LE length prefix.
    std::uint8_t lenbuf[4];
    ssl_read_exact(impl_->ssl, lenbuf, sizeof(lenbuf), /*at_frame_start=*/true);
    const std::uint32_t len = static_cast<std::uint32_t>(lenbuf[0]) |
                              (static_cast<std::uint32_t>(lenbuf[1]) << 8) |
                              (static_cast<std::uint32_t>(lenbuf[2]) << 16) |
                              (static_cast<std::uint32_t>(lenbuf[3]) << 24);

    // 2) Enforce the 8 KiB cap BEFORE allocating — an oversize prefix from an
    //    untrusted peer must not induce a huge allocation (SAD §5.1).
    if (len > kMaxFrameBytes) {
        throw TlsError("frame length " + std::to_string(len) +
                       " exceeds max " + std::to_string(kMaxFrameBytes));
    }

    // 3) Read exactly len payload bytes. Zero-length frames are legal.
    Bytes payload(len);
    if (len > 0) {
        ssl_read_exact(impl_->ssl, payload.data(), len, /*at_frame_start=*/false);
    }
    return payload;
}

void Session::write_frame(const Bytes& payload) {
    if (!impl_ || !impl_->ssl) throw TlsError("write_frame on closed session");
    if (payload.size() > kMaxFrameBytes) {
        throw TlsError("payload " + std::to_string(payload.size()) +
                       " exceeds max " + std::to_string(kMaxFrameBytes));
    }

    // Prefix + payload in one contiguous buffer so it goes out as a unit.
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    Bytes frame;
    frame.reserve(4 + payload.size());
    frame.push_back(static_cast<std::uint8_t>(len & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());

    std::size_t sent = 0;
    while (sent < frame.size()) {
        int w = SSL_write(impl_->ssl, frame.data() + sent,
                          static_cast<int>(frame.size() - sent));
        if (w > 0) {
            sent += static_cast<std::size_t>(w);
            continue;
        }
        throw_tls("SSL_write");
    }
}

void Session::close() {
    if (!impl_) return;
    if (impl_->ssl) {
        SSL_shutdown(impl_->ssl);
        SSL_free(impl_->ssl);
        impl_->ssl = nullptr;
    }
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
}

// ---- TlsListener ------------------------------------------------------------

struct TlsListener::Impl {
    SSL_CTX* ctx = nullptr;
    int listen_fd = -1;
    std::uint16_t port = 0;

    ~Impl() {
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }
};

TlsListener::TlsListener(const ListenConfig& cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->ctx = make_server_ctx(cfg);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw_errno("socket");

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    if (::inet_pton(AF_INET, cfg.bind_addr.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw TlsError("invalid bind_addr '" + cfg.bind_addr + "'");
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        throw_errno("bind");
    }
    if (::listen(fd, cfg.backlog) != 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        throw_errno("listen");
    }

    // Resolve the actual bound port (matters when cfg.port == 0 → ephemeral).
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        impl_->port = ntohs(bound.sin_port);
    } else {
        impl_->port = cfg.port;
    }

    impl_->listen_fd = fd;
}

TlsListener::TlsListener(TlsListener&&) noexcept = default;
TlsListener& TlsListener::operator=(TlsListener&&) noexcept = default;
TlsListener::~TlsListener() = default;

std::uint16_t TlsListener::local_port() const {
    return impl_ ? impl_->port : 0;
}

void TlsListener::close() {
    if (impl_ && impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
}

Session TlsListener::accept() {
    if (!impl_ || impl_->listen_fd < 0) throw TlsError("accept on closed listener");

    sockaddr_storage peer_ss{};
    socklen_t plen = sizeof(peer_ss);
    int cfd = ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&peer_ss),
                       &plen);
    if (cfd < 0) throw_errno("accept");

    // TCP_NODELAY: auth frames are tiny and latency-sensitive; don't Nagle them.
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    auto simpl = std::make_unique<Session::Impl>();
    simpl->fd = cfd;
    simpl->peer = format_peer(peer_ss);

    simpl->ssl = SSL_new(impl_->ctx);
    if (!simpl->ssl) throw_tls("SSL_new");  // Impl dtor closes cfd
    if (SSL_set_fd(simpl->ssl, cfd) != 1) throw_tls("SSL_set_fd");

    int r = SSL_accept(simpl->ssl);
    if (r != 1) {
        // Handshake failed — e.g. a client that can't do TLS 1.3 (we enforce the
        // 1.3 floor), or a bad/aborted handshake. Impl dtor frees ssl + fd.
        throw TlsError("TLS handshake failed: " + openssl_errors());
    }

    return Session(std::move(simpl));
}

}  // namespace meridian::net
