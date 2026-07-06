// SPDX-License-Identifier: Apache-2.0
//
// meridian-net — TLS 1.3 listener for authd's IF-1 auth protocol.
//
// CLEAN-ROOM: implemented from the published specifications only — the Project
// Meridian server SAD (§5.1 IF-1 transport: "TLS 1.3 over TCP", framing
// "u32 LE length | FlatBuffer root table", max 8 KiB; §2.1 "one acceptor + small
// IO pool") and the OpenSSL public API documentation. No GPL source (CMaNGOS /
// TrinityCore or otherwise) was consulted. See CONTRIBUTING.md.
//
// Scope: this component terminates TLS 1.3 and speaks IF-1's length framing. It
// deals in OPAQUE byte buffers — the FlatBuffer payload (ClientHello, SrpStart,
// … SessionGrant) is the caller's concern and is NOT parsed here. Transport is
// deliberately dependency-light: plain BSD sockets + OpenSSL, no Asio/Boost at M0
// (the SAD's "small IO pool" is realised by the caller running accept() on an
// acceptor thread and handing each Session to a worker; the library imposes no
// threading model of its own beyond being safe to use that way).

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace meridian::net {

using Bytes = std::vector<std::uint8_t>;

// IF-1 framing bound (server SAD §5.1): the u32 LE length prefix excludes itself
// and MUST NOT exceed 8 KiB. A prefix over this is rejected before any allocation
// of the payload buffer — an untrusted peer cannot induce a huge alloc.
inline constexpr std::uint32_t kMaxFrameBytes = 8u * 1024u;  // 8192

// Thrown for any transport-level failure: bind/listen/accept errors, TLS
// handshake failure, a framing violation (oversize length prefix), or a peer
// that closes mid-frame. Carries a human-readable reason for logging.
class TlsError : public std::runtime_error {
public:
    explicit TlsError(const std::string& what) : std::runtime_error(what) {}
};

// Signals an orderly end-of-stream: the peer closed the connection cleanly at a
// frame boundary (before a new length prefix). Distinct from TlsError so a
// serve loop can treat "peer done" differently from "peer misbehaved".
class ConnectionClosed : public std::runtime_error {
public:
    ConnectionClosed() : std::runtime_error("connection closed by peer") {}
};

// Configuration for a TLS 1.3 listener. Cert + key are file paths (PEM), per the
// SAD's "server cert from realm operator PKI" — the operator provisions them out
// of band and points authd at them via config.
struct ListenConfig {
    std::string cert_path;              // PEM server certificate (chain allowed)
    std::string key_path;               // PEM private key for cert_path
    std::string bind_addr = "0.0.0.0";  // interface to bind; "0.0.0.0" = all v4
    std::uint16_t port = 7100;          // IF-1 default port (SAD §5.1)
    int backlog = 128;                  // listen() backlog
};

// A single accepted, TLS-1.3-terminated connection. Speaks IF-1 length framing
// over the encrypted channel. Move-only (owns an fd + an SSL object); closing is
// idempotent and also happens in the destructor. Not thread-safe: one Session is
// driven by one thread at a time (the intended "hand each Session to a worker"
// pattern gives each worker its own Session, so no sharing is required).
class Session {
public:
    ~Session();
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Read exactly one IF-1 frame: a u32 LE length prefix followed by that many
    // payload bytes. Returns the payload (prefix stripped). Throws:
    //   - TlsError if the length prefix exceeds kMaxFrameBytes (8 KiB) — checked
    //     BEFORE allocating the payload buffer,
    //   - ConnectionClosed on a clean EOF at the start of a frame,
    //   - TlsError on a truncated frame or any TLS/socket error.
    Bytes read_frame();

    // Write one IF-1 frame: prepends the u32 LE length prefix and sends prefix +
    // payload atomically (from the caller's view — a single logical frame).
    // Throws TlsError if payload exceeds kMaxFrameBytes or on any write error.
    void write_frame(const Bytes& payload);

    // Peer address "ip:port" for logging / IP-ban checks (SAD §2.1). Best effort.
    const std::string& peer() const;

    // Negotiated TLS version string from OpenSSL (e.g. "TLSv1.3"). Useful for the
    // enforcement assertion and for diagnostics.
    std::string tls_version() const;

    // Idempotent orderly shutdown: TLS close_notify then socket close.
    void close();

private:
    friend class TlsListener;
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// TLS 1.3 acceptor. Binds + listens on construction; accept() blocks for one
// connection, performs the TLS handshake (enforcing TLS 1.3 as the minimum via
// SSL_CTX_set_min_proto_version(TLS1_3_VERSION)), and returns a ready Session.
// Move-only; owns the listening socket + the shared SSL_CTX.
class TlsListener {
public:
    // Loads cert/key, builds a TLS-1.3-minimum SSL_CTX, then binds + listens.
    // Throws TlsError on any failure (bad cert/key, bind in use, etc.).
    explicit TlsListener(const ListenConfig& cfg);
    ~TlsListener();
    TlsListener(TlsListener&&) noexcept;
    TlsListener& operator=(TlsListener&&) noexcept;
    TlsListener(const TlsListener&) = delete;
    TlsListener& operator=(const TlsListener&) = delete;

    // Block until one client connects, complete the TLS 1.3 handshake, and hand
    // back the ready Session. A handshake that would negotiate below TLS 1.3 is
    // refused by OpenSSL and surfaces as TlsError. The caller typically runs this
    // on an acceptor thread and dispatches each Session to a worker (SAD §2.1).
    Session accept();

    // The actual bound port. Equals cfg.port unless port 0 was requested, in
    // which case the OS assigns an ephemeral port (used by the loopback test).
    std::uint16_t local_port() const;

    // Stop accepting: closes the listening socket. In-flight Sessions are
    // unaffected. Idempotent.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meridian::net
