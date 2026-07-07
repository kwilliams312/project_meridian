// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-agnostic client net core: the frame-level transport
// seam (issue #95).
//
// The net core speaks in FRAMES (a payload), not bytes: a transport owns the socket
// / TLS session and the u32-LE length framing (framing.h). This interface is the
// SAME shape as the login core's ILoginTransport
// (client/gdextension/meridian/src/login_core.h) so a caller can drive either behind
// one seam and a mock can replace a real socket in tests.

#ifndef MERIDIAN_CLIENTNET_TRANSPORT_H
#define MERIDIAN_CLIENTNET_TRANSPORT_H

#include <optional>

#include "meridian/clientnet/framing.h"  // Bytes

namespace meridian::clientnet {

// Frame-level byte I/O over an already-established connection.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Send one frame: the implementation prepends the u32-LE length prefix (max
    // kMaxFrameBytes) and writes prefix+payload. Returns false on any write error /
    // oversize payload.
    virtual bool send_frame(const Bytes& payload) = 0;

    // Receive exactly one frame: read the u32-LE length, then that many payload
    // bytes; return the payload (prefix stripped). std::nullopt on a clean EOF or
    // any read/framing error.
    virtual std::optional<Bytes> recv_frame() = 0;

    // Non-blocking-with-timeout receive. Distinguishes "no frame yet" (a read
    // timeout, `would_block` set true) from "peer closed / error" (`would_block`
    // false). Default: no timeout support — behaves exactly like recv_frame().
    // Concrete socket transports override it to honour the timeout set via
    // set_recv_timeout_ms().
    virtual std::optional<Bytes> recv_frame_nb(bool& would_block) {
        would_block = false;
        return recv_frame();
    }

    // Set the receive timeout (milliseconds) applied by recv_frame_nb(); 0 disables
    // it (blocking). Default no-op for transports without a real socket (a mock).
    virtual void set_recv_timeout_ms(unsigned /*ms*/) {}
};

}  // namespace meridian::clientnet

#endif  // MERIDIAN_CLIENTNET_TRANSPORT_H
