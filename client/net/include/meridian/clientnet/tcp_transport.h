// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-agnostic client net core: plain-TCP frame transport
// (issue #95).
//
// A blocking BSD-socket ITransport speaking the u32-LE length framing (framing.h).
// This is the plaintext IF-2 transport the bot uses against worldd at M0 (worldd's
// world channel is TCP, not TLS — schema/net/world.fbs: "TCP, default port 7200 …
// Not TLS at M0"). It mirrors the socket + framing layer of the proven
// TlsLoginTransport (client/gdextension/meridian/src/login_transport.cpp) MINUS the
// TLS wrap. Auth (IF-1) uses TlsClientTransport instead (tls_client.h).
//
// Owns an fd; move-disabled (RAII close in the destructor). Not thread-safe: one
// connection is driven by one thread.

#ifndef MERIDIAN_CLIENTNET_TCP_TRANSPORT_H
#define MERIDIAN_CLIENTNET_TCP_TRANSPORT_H

#include <cstdint>
#include <optional>
#include <string>

#include "meridian/clientnet/transport.h"

namespace meridian::clientnet {

class TcpTransport final : public ITransport {
public:
    // Connect a TCP socket to host:port (tries each resolved v4/v6 address). On
    // failure the transport is left not-ok(); the caller checks ok() before use.
    TcpTransport(const std::string& host, std::uint16_t port);
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // True once the TCP connect succeeded.
    bool ok() const { return connected_; }

    // A human-readable reason the connect failed (empty when ok()).
    const std::string& error() const { return error_; }

    // ITransport.
    bool send_frame(const Bytes& payload) override;
    std::optional<Bytes> recv_frame() override;
    std::optional<Bytes> recv_frame_nb(bool& would_block) override;
    void set_recv_timeout_ms(unsigned ms) override;

    // Close the socket (idempotent; also runs in the destructor).
    void close();

private:
    bool write_all(const std::uint8_t* buf, std::size_t n);
    bool read_all(std::uint8_t* buf, std::size_t n);
    bool read_all_timed(std::uint8_t* buf, std::size_t n, bool& timed_out);

    int fd_ = -1;
    bool connected_ = false;
    std::string error_;
    unsigned recv_timeout_ms_ = 0;  // 0 = blocking (SO_RCVTIMEO unset)
};

}  // namespace meridian::clientnet

#endif  // MERIDIAN_CLIENTNET_TCP_TRANSPORT_H
