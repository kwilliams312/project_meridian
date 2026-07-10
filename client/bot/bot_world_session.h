// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — CLIENT-side IF-2 world session: the piece #99 did not build.
// The bot (and, later, the Godot client) needs the OTHER half of worldd's #84
// session establishment: connect to worldd, present the grant via WorldHello,
// read HandshakeOk, and then exchange IF-2 frames — with a per-session AEAD
// channel keyed off the SessionGrant.session_key that MIRRORS worldd #84 EXACTLY
// so client↔worldd crypto agrees byte-for-byte.
//
// CLEAN-ROOM: implemented from the wire contract (schema/net/world.fbs), the
// server SAD §5.2 (IF-2 framing + the per-session AEAD channel), and the SERVER
// implementation as the interop reference (server/worldd/world_session.cpp #84 for
// the HKDF + nonce scheme, server/worldd/world_dispatch.cpp #82/#83 for the frame
// codec). No GPL source consulted (CONTRIBUTING.md).
//
// TWO concerns, both the CLIENT half of the WorldHello handshake + session:
//
//   1. IF-2 FRAME CODEC. An IF-2 frame body is  [ u16 opcode LE ][ u64 seq LE ]
//      [ payload ]  (world_dispatch.cpp encode_frame/decode_frame). This whole
//      body is the payload the u32-LE-length transport frames on the wire. The
//      codec here encodes/decodes that inner header — the exact mirror of the
//      server's.
//
//   2. AEAD SESSION (SAD §5.2, matching worldd #84 world_session.cpp). After a
//      valid grant BOTH ends derive a ChaCha20-Poly1305 channel off the 32-byte
//      session_key:
//        k_c2s, k_s2c = HKDF-SHA256(session_key, salt="", "meridian-world-v1"∥dir)
//        nonce        = [ dir : 1 ][ 0 : 3 ][ seq : 8, big-endian ]
//      This ClientWorldSession owns the two derived keys + two monotonic counters
//      and exposes seal()/open() so it can wrap/unwrap IF-2 payloads. The scheme
//      is byte-identical to the server's WorldSession — the AEAD-interop unit test
//      proves a frame this client seals opens under the SERVER's WorldSession and
//      vice-versa.
//
// M0 WIRE REALITY (honest, read from worldd #84 on main): at M0 worldd writes the
// IF-2 payload as PLAINTEXT on the wire (server/worldd/world_state.cpp
// SessionEgress::emit calls seal() only to ADVANCE the s2c counter, then writes
// the plaintext body; the inbound path decode_frame()s a plaintext FlatBuffer and
// never open()s). Confidentiality on the wire is TLS 1.3; the AEAD wrap is the
// documented seam that flips on later. So the BOT sends/receives PLAINTEXT IF-2
// bodies to interoperate with the real server today, while ClientWorldSession is
// built + proven so the seam is client-ready the instant worldd flips it on.

#ifndef MERIDIAN_BOT_WORLD_SESSION_H
#define MERIDIAN_BOT_WORLD_SESSION_H

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace meridian::bot {

using Bytes = std::vector<std::uint8_t>;

// ChaCha20-Poly1305 sizes (RFC 8439), fixed by the cipher + SAD §5.2. These match
// server/worldd/world_session.h kAead*Bytes exactly.
inline constexpr std::size_t kAeadKeyBytes = 32;
inline constexpr std::size_t kAeadNonceBytes = 12;
inline constexpr std::size_t kAeadTagBytes = 16;

// Wire direction — names the HKDF info label and the top nonce byte. Values match
// server WorldSession::Direction (c2s=0, s2c=1) so the two ends key identically.
enum class Direction : std::uint8_t {
    kClientToServer = 0,
    kServerToClient = 1,
};

// The IF-2 opcodes the bot uses (schema/net/world.fbs Opcode). Exposed so the
// codec + tests do not hard-code magic numbers.
inline constexpr std::uint16_t kOpWorldHello     = 0x0001;  // C→S
inline constexpr std::uint16_t kOpHandshakeOk    = 0x0002;  // S→C
inline constexpr std::uint16_t kOpDisconnect     = 0x0003;  // S→C
inline constexpr std::uint16_t kOpClockSync      = 0x0004;  // C↔S
inline constexpr std::uint16_t kOpCharListReq    = 0x0010;  // C→S
inline constexpr std::uint16_t kOpCharListResp   = 0x0011;  // S→C
inline constexpr std::uint16_t kOpCharCreateReq  = 0x0012;  // C→S
inline constexpr std::uint16_t kOpCharCreateResp = 0x0013;  // S→C
inline constexpr std::uint16_t kOpEnterWorldReq  = 0x0016;  // C→S  spawn as an OWNED character
inline constexpr std::uint16_t kOpEnterWorldResp = 0x0017;  // S→C  typed enter result
inline constexpr std::uint16_t kOpMovementIntent = 0x1001;  // C→S
inline constexpr std::uint16_t kOpMovementState  = 0x1002;  // S→C
inline constexpr std::uint16_t kOpEntityEnter    = 0x2001;  // S→C
inline constexpr std::uint16_t kOpEntityUpdate   = 0x2002;  // S→C
inline constexpr std::uint16_t kOpEntityLeave    = 0x2003;  // S→C

// The IF-2 in-frame header size: u16 opcode + u64 seq (world_dispatch.h
// kFrameHeaderBytes). A frame body shorter than this is malformed.
inline constexpr std::size_t kFrameHeaderBytes = sizeof(std::uint16_t) + sizeof(std::uint64_t);

// A decoded IF-2 frame: opcode + seq + payload (the FlatBuffer body). Mirrors
// server/worldd/world_dispatch.h Frame.
struct WorldFrame {
    std::uint16_t opcode = 0;
    std::uint64_t seq = 0;
    Bytes payload;  // the FlatBuffer table bytes (plaintext at M0)
};

// ---------------------------------------------------------------------------
// IF-2 frame codec (mirrors worldd's world_dispatch.cpp encode_frame/decode_frame)
// ---------------------------------------------------------------------------

// Wrap `payload` in the IF-2 frame header: u16 opcode LE ‖ u64 seq LE ‖ payload.
// The result is the body the u32-LE-length transport frames on the wire.
Bytes encode_world_frame(std::uint16_t opcode, std::uint64_t seq, const Bytes& payload);

// Decode a received IF-2 frame body (transport-frame payload) into opcode/seq/
// payload. std::nullopt if the body is shorter than the IF-2 header.
std::optional<WorldFrame> decode_world_frame(const Bytes& frame);

// ---------------------------------------------------------------------------
// ClientWorldSession — the per-session AEAD channel (mirror of worldd #84)
// ---------------------------------------------------------------------------

// A per-session ChaCha20-Poly1305 channel keyed off the 32-byte session_key,
// byte-identical to server/worldd/world_session.cpp WorldSession so a frame one
// end seals the other end opens. See the file header for the HKDF + nonce scheme.
//
// Not thread-safe: one session is driven by one connection's thread (the bot is
// synchronous). The bot's own c2s counter is authoritative for the frames it
// sends; the wire seq of an inbound frame supplies the nonce for open().
class ClientWorldSession {
public:
    // Derive both direction keys from `session_key` (must be exactly 32 bytes).
    // ok() is false on a wrong-length key or an OpenSSL HKDF failure — a session
    // must never proceed with mis-derived keys.
    explicit ClientWorldSession(const Bytes& session_key);

    // True once both direction keys derived successfully.
    bool ok() const { return ok_; }

    // Seal `plaintext` for transmission in `dir` (the sender's own direction).
    // Returns ciphertext‖tag; `out_seq` receives the sequence it was sealed under
    // (the value that goes in the IF-2 frame header). `aad` binds extra
    // authenticated data (empty at M0). Advances this direction's counter.
    // Returns std::nullopt on counter exhaustion or an OpenSSL failure.
    std::optional<Bytes> seal(Direction dir, const Bytes& plaintext, const Bytes& aad,
                              std::uint64_t& out_seq);

    // Open a frame received on `dir` sealed under `seq` with `aad`. Returns the
    // plaintext on a valid tag, std::nullopt on any authentication failure. Does
    // NOT advance a counter (the wire seq supplies the nonce — mirrors worldd).
    std::optional<Bytes> open(Direction dir, const Bytes& ciphertext_and_tag,
                              std::uint64_t seq, const Bytes& aad);

    // The next sequence seal() will use for `dir` (test/diagnostic).
    std::uint64_t next_seq(Direction dir) const;

    // The derived 32-byte key for a direction (test/diagnostic — lets the AEAD
    // interop test assert client + server derive the SAME keys).
    const std::array<std::uint8_t, kAeadKeyBytes>& key(Direction dir) const;

private:
    std::array<std::uint8_t, kAeadKeyBytes> k_c2s_{};
    std::array<std::uint8_t, kAeadKeyBytes> k_s2c_{};
    std::uint64_t seq_c2s_ = 0;
    std::uint64_t seq_s2c_ = 0;
    bool ok_ = false;
};

}  // namespace meridian::bot

#endif  // MERIDIAN_BOT_WORLD_SESSION_H
