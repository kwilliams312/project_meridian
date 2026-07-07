// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — real TLS 1.3 client transport for the IF-1 login core
// (issue #99). The engine-free ILoginTransport implementation over an OpenSSL
// TLS 1.3 client socket, mirroring the SERVER's meridian-net TLS listener
// (server/libs/net) on the other side of the wire: TLS 1.3 minimum, IF-1 u32-LE
// length framing (max 8 KiB), FlatBuffer payloads.
//
// This is the transport the GDExtension binding (meridian_login.*) and the live-
// authd integration test use to actually reach authd. It is deliberately socket-
// level (plain BSD sockets + OpenSSL, no Godot StreamPeerTLS) so the whole net
// path stays engine-free and testable without a Godot runtime — the same
// dependency-light choice server/libs/net made (SAD §5.1 "TLS 1.3 over TCP").
//
// CLEAN-ROOM: from the OpenSSL public API docs + the server-side net library as
// the interop reference. No GPL source consulted (CONTRIBUTING.md).
//
// M0 TLS TRUST NOTE (honest): at M0 the client does NOT verify the authd server
// certificate chain (SSL_VERIFY_NONE) — the realm operator PKI / cert pinning is
// a follow-up (Client SAD §5.1). Confidentiality on the wire is still TLS 1.3;
// SRP-6a mutual auth (the M2 check in login_core) authenticates the SERVER's
// possession of the verifier independently of the TLS cert. Wiring cert
// verification here is a one-call change (SSL_CTX_set_verify + a trust store).

#ifndef MERIDIAN_LOGIN_TRANSPORT_H
#define MERIDIAN_LOGIN_TRANSPORT_H

#include <cstdint>
#include <optional>
#include <string>

#include "login_core.h"

namespace meridian::login {

// A TLS 1.3 client connection to authd (IF-1) speaking the u32-LE length framing.
// Owns an fd + an SSL object; move-disabled (RAII close in the destructor). Not
// thread-safe: one connection is driven by one thread (the login flow is
// synchronous, off Godot's main thread — Client SAD §6.1).
class TlsLoginTransport final : public ILoginTransport {
public:
    // Connect to host:port and complete the TLS 1.3 handshake. On any failure
    // (DNS, connect, handshake) the transport is left not-ok(); the caller checks
    // ok() before driving run_login (which returns kConnectFailed if frames can't
    // flow). `host` may be an IPv4/IPv6 literal or a DNS name.
    TlsLoginTransport(const std::string& host, std::uint16_t port);
    ~TlsLoginTransport() override;

    TlsLoginTransport(const TlsLoginTransport&) = delete;
    TlsLoginTransport& operator=(const TlsLoginTransport&) = delete;

    // True once the TCP connect + TLS 1.3 handshake both succeeded.
    bool ok() const { return connected_; }

    // The negotiated TLS version string (e.g. "TLSv1.3"), or "" if not connected.
    std::string tls_version() const;

    // A human-readable reason the connect failed (empty when ok()).
    const std::string& error() const { return error_; }

    // ILoginTransport.
    bool send_frame(const Bytes& payload) override;
    std::optional<Bytes> recv_frame() override;

    // Close the TLS session + socket (idempotent; also runs in the destructor).
    void close();

private:
    bool write_all(const std::uint8_t* buf, std::size_t n);
    bool read_all(std::uint8_t* buf, std::size_t n);

    void* ctx_ = nullptr;   // SSL_CTX* (void to keep OpenSSL out of the header)
    void* ssl_ = nullptr;   // SSL*
    int fd_ = -1;
    bool connected_ = false;
    std::string error_;
};

}  // namespace meridian::login

#endif  // MERIDIAN_LOGIN_TRANSPORT_H
