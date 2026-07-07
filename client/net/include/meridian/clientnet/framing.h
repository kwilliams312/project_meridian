// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-agnostic client net core: length-prefixed transport
// framing (issue #95).
//
// This is the OUTER wire framing every Meridian channel uses beneath the payload:
//
//     u32 LE length ‖ payload
//
// The length prefix EXCLUDES itself and is capped at kMaxFrameBytes (8 KiB). This
// is the EXACT framing the proven stack already speaks — the client login transport
// (client/gdextension/meridian/src/login_transport.cpp send_frame/recv_frame) and
// the server TLS listener (server/libs/net, meridian-net kMaxFrameBytes). This
// header does NOT invent a new format; it factors the SAME byte layout out of the
// socket code into a pure, socket-free primitive so the bot, a doctest suite, and
// (later) the Godot client can share one deframer with one set of tests.
//
// PURE + SOCKET-FREE: these functions only touch byte buffers. A caller owns the
// socket/TLS session and feeds bytes in / writes bytes out. That is what makes the
// framing unit-testable with known-answer vectors and no live server.
//
// CLEAN-ROOM: the format is documented in schema/net/{auth,world}.fbs and the
// server SAD §5.1/§5.2; the implementation is factored from THIS repo's own proven
// login_transport.cpp / meridian-net. No GPL source consulted (CONTRIBUTING.md).

#ifndef MERIDIAN_CLIENTNET_FRAMING_H
#define MERIDIAN_CLIENTNET_FRAMING_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace meridian::clientnet {

using Bytes = std::vector<std::uint8_t>;

// Framing bound (server SAD §5.1 / meridian-net kMaxFrameBytes): the u32-LE length
// prefix excludes itself and MUST NOT exceed 8 KiB. A hostile/broken peer
// advertising more than this is rejected BEFORE any allocation. (IF-2 allows larger
// caps at M2 when it terminates at gatewayd; at M0 both channels ride the 8 KiB
// TLS framing, which is what the bot + login transport enforce today.)
inline constexpr std::uint32_t kMaxFrameBytes = 8u * 1024u;

// The size of the length prefix itself.
inline constexpr std::size_t kLengthPrefixBytes = sizeof(std::uint32_t);

// Prepend the u32-LE length prefix to `payload`, producing the exact bytes that go
// on the wire: [ len:u32 LE ][ payload ]. Returns std::nullopt if `payload` exceeds
// kMaxFrameBytes (an oversize frame is a programming/protocol error, never framed).
std::optional<Bytes> frame_message(const Bytes& payload);

// Read the u32-LE length prefix from the first kLengthPrefixBytes of `buf`. Callers
// that read a stream byte-by-byte use this to learn how many payload bytes follow.
// Returns std::nullopt if `buf` is shorter than the prefix. Does NOT range-check the
// value against kMaxFrameBytes — the caller does that (see length_is_valid).
std::optional<std::uint32_t> read_length_prefix(const Bytes& buf);

// Whether an advertised (untrusted) frame length is acceptable: 0..kMaxFrameBytes.
inline bool length_is_valid(std::uint32_t len) { return len <= kMaxFrameBytes; }

// ---------------------------------------------------------------------------
// FrameReader — incremental deframer for a byte stream.
// ---------------------------------------------------------------------------
//
// A stream transport (TCP/TLS) delivers bytes in arbitrary chunks that do NOT
// align to frame boundaries: one read may carry half a length prefix, or three
// whole frames plus a partial fourth. FrameReader buffers those bytes and yields
// COMPLETE payloads (prefix stripped) one at a time. This is the piece a
// non-blocking or chunked transport needs and that a strict "read exactly N bytes"
// blocking helper (login_transport.cpp read_all) does not provide.
//
// Usage:
//   FrameReader r;
//   r.feed(bytes_from_socket);               // append whatever arrived
//   while (auto payload = r.next()) { ... }   // drain every complete frame
//
// A frame whose advertised length exceeds kMaxFrameBytes puts the reader into a
// permanent error state (error() == true): the stream is corrupt/hostile and must
// be torn down. next() returns std::nullopt forever once errored.
class FrameReader {
public:
    // Append freshly-read stream bytes to the internal buffer.
    void feed(const std::uint8_t* data, std::size_t n);
    void feed(const Bytes& data) { feed(data.data(), data.size()); }

    // Pop the next COMPLETE frame payload (length prefix stripped), or std::nullopt
    // if a whole frame is not yet buffered (need more bytes) or the reader errored.
    std::optional<Bytes> next();

    // True once an oversize length prefix was seen — the stream is unusable.
    bool error() const { return error_; }

    // Bytes currently buffered but not yet formed into a complete frame.
    std::size_t buffered() const { return buf_.size() - consumed_; }

private:
    void compact();  // drop already-yielded leading bytes when they pile up

    Bytes buf_;
    std::size_t consumed_ = 0;  // how many leading bytes of buf_ are already yielded
    bool error_ = false;
};

}  // namespace meridian::clientnet

#endif  // MERIDIAN_CLIENTNET_FRAMING_H
