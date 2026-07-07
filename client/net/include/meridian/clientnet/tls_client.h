// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-agnostic client net core: TLS 1.3 client transport
// (issue #95).
//
// The auth channel (IF-1, authd) is TLS 1.3 over TCP (schema/net/auth.fbs). This
// ITransport is an OpenSSL TLS 1.3 client over a plain BSD socket speaking the
// u32-LE length framing (framing.h) — the SAME dependency-light choice
// server/libs/net (the authd listener) and the proven client TlsLoginTransport
// (client/gdextension/meridian/src/login_transport.cpp) make.
//
// CRYPTO STACK NOTE (issue #95 scope vs. codebase reality): the #95 brief names
// mbedTLS for this client. The ENTIRE proven Meridian stack — authd, worldd,
// server/libs/net, the bot, and TlsLoginTransport — is OpenSSL, and the byte-exact
// interop surfaces (SRP-6a, HMAC, HKDF-SHA256, ChaCha20-Poly1305 AEAD) are OpenSSL
// EVP. TLS 1.3 is standardized on the wire, so an mbedTLS client WOULD interoperate
// with the OpenSSL authd; but adding mbedTLS forks the crypto stack, needs new
// dependency provisioning (setup-macos.sh installs only openssl@3), and duplicates
// this already-proven transport for no interop gain. So this mirrors
// TlsLoginTransport on OpenSSL to match the stack EXACTLY (the "cross-track drift =
// broken E2E" constraint). Swapping the TLS provider later is a contained change
// behind this class; the framing/AEAD it wraps are provider-independent.
//
// M0 TLS TRUST NOTE (honest, matches TlsLoginTransport): at M0 the client does NOT
// verify the authd certificate chain (SSL_VERIFY_NONE). Confidentiality is TLS 1.3;
// SRP-6a mutual auth (the M2 check in the login core) authenticates the server's
// possession of the verifier independently of the cert. Cert pinning / operator PKI
// is a follow-up (Client SAD §5.1); wiring it here is a one-call change
// (SSL_CTX_set_verify + a trust store).
//
// Owns an fd + an SSL object; move-disabled (RAII close in the destructor). Not
// thread-safe: one connection is driven by one thread.

#ifndef MERIDIAN_CLIENTNET_TLS_CLIENT_H
#define MERIDIAN_CLIENTNET_TLS_CLIENT_H

#include <cstdint>
#include <optional>
#include <string>

#include "meridian/clientnet/transport.h"

namespace meridian::clientnet {

class TlsClientTransport final : public ITransport {
public:
    // Connect to host:port and complete the TLS 1.3 handshake. On any failure (DNS,
    // connect, handshake) the transport is left not-ok(); the caller checks ok()
    // before use. `host` may be an IPv4/IPv6 literal or a DNS name.
    TlsClientTransport(const std::string& host, std::uint16_t port);
    ~TlsClientTransport() override;

    TlsClientTransport(const TlsClientTransport&) = delete;
    TlsClientTransport& operator=(const TlsClientTransport&) = delete;

    // True once the TCP connect + TLS 1.3 handshake both succeeded.
    bool ok() const { return connected_; }

    // The negotiated TLS version string (e.g. "TLSv1.3"), or "" if not connected.
    std::string tls_version() const;

    // A human-readable reason the connect failed (empty when ok()).
    const std::string& error() const { return error_; }

    // ITransport.
    bool send_frame(const Bytes& payload) override;
    std::optional<Bytes> recv_frame() override;
    std::optional<Bytes> recv_frame_nb(bool& would_block) override;
    void set_recv_timeout_ms(unsigned ms) override;

    // Close the TLS session + socket (idempotent; also runs in the destructor).
    void close();

private:
    bool write_all(const std::uint8_t* buf, std::size_t n);
    bool read_all(std::uint8_t* buf, std::size_t n);
    bool read_all_timed(std::uint8_t* buf, std::size_t n, bool& timed_out);

    void* ctx_ = nullptr;   // SSL_CTX* (void to keep OpenSSL out of the header)
    void* ssl_ = nullptr;   // SSL*
    int fd_ = -1;
    bool connected_ = false;
    std::string error_;
    unsigned recv_timeout_ms_ = 0;  // 0 = blocking (SO_RCVTIMEO unset)
};

}  // namespace meridian::clientnet

#endif  // MERIDIAN_CLIENTNET_TLS_CLIENT_H
