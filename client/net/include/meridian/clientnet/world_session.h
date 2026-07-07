// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-agnostic client net core: the per-session IF-2 AEAD
// channel (issue #95).
//
// After a valid SessionGrant, both ends derive a ChaCha20-Poly1305 channel off the
// 32-byte session_key, byte-identical to the server (server/worldd/world_session.cpp
// #84) and the bot (client/bot/bot_world_session.cpp), so a frame one end seals the
// other end opens:
//
//   k_c2s, k_s2c = HKDF-SHA256(session_key, salt="", "meridian-world-v1" ‖ dir)
//   nonce        = [ dir : 1 ][ 0 : 3 ][ seq : 8, big-endian ]
//
// (SAD §5.2. dir = 0 for client→server, 1 for server→client.) This class owns the
// two derived keys + two monotonic counters and exposes seal()/open() to wrap/unwrap
// IF-2 payloads.
//
// M0 WIRE REALITY (honest): at M0 worldd writes the IF-2 payload as PLAINTEXT
// (confidentiality on the wire is TLS 1.3; the AEAD wrap is the documented seam that
// flips on later). So the bot sends/receives plaintext IF-2 bodies today while this
// session is built + proven so the seam is client-ready the instant worldd flips it
// on. The AEAD-interop test proves a frame this class seals opens under an
// INDEPENDENT reference implementation of the same scheme.
//
// CLEAN-ROOM: from SAD §5.2 + the OpenSSL EVP public API + this repo's own server /
// bot as the interop reference. No GPL source consulted (CONTRIBUTING.md).

#ifndef MERIDIAN_CLIENTNET_WORLD_SESSION_H
#define MERIDIAN_CLIENTNET_WORLD_SESSION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "meridian/clientnet/framing.h"  // Bytes

namespace meridian::clientnet {

// ChaCha20-Poly1305 sizes (RFC 8439), fixed by the cipher + SAD §5.2. Match
// server/worldd/world_session.h and the bot's kAead*Bytes exactly.
inline constexpr std::size_t kAeadKeyBytes = 32;
inline constexpr std::size_t kAeadNonceBytes = 12;
inline constexpr std::size_t kAeadTagBytes = 16;

// Wire direction — names the HKDF info label and the top nonce byte. Values match
// the server/bot Direction (c2s=0, s2c=1) so both ends key identically.
enum class Direction : std::uint8_t {
    kClientToServer = 0,
    kServerToClient = 1,
};

// A per-session ChaCha20-Poly1305 channel keyed off the 32-byte session_key. Not
// thread-safe: one session is driven by one connection's thread.
class WorldSession {
public:
    // Derive both direction keys from `session_key` (must be exactly 32 bytes).
    // ok() is false on a wrong-length key or an HKDF failure — a session must never
    // proceed with mis-derived keys.
    explicit WorldSession(const Bytes& session_key);

    // True once both direction keys derived successfully.
    bool ok() const { return ok_; }

    // Seal `plaintext` for transmission in `dir` (the sender's own direction).
    // Returns ciphertext‖tag; `out_seq` receives the sequence it was sealed under
    // (the value that goes in the IF-2 frame header). `aad` binds extra
    // authenticated data (empty at M0). Advances this direction's counter. Returns
    // std::nullopt on counter exhaustion or a crypto failure.
    std::optional<Bytes> seal(Direction dir, const Bytes& plaintext, const Bytes& aad,
                              std::uint64_t& out_seq);

    // Open a frame received on `dir` sealed under `seq` with `aad`. Returns the
    // plaintext on a valid tag, std::nullopt on any authentication failure. Does
    // NOT advance a counter (the wire seq supplies the nonce — mirrors the server).
    std::optional<Bytes> open(Direction dir, const Bytes& ciphertext_and_tag,
                              std::uint64_t seq, const Bytes& aad);

    // The next sequence seal() will use for `dir` (test/diagnostic).
    std::uint64_t next_seq(Direction dir) const;

    // The derived 32-byte key for a direction (test/diagnostic — lets an interop
    // test assert two ends derive the SAME keys).
    const std::array<std::uint8_t, kAeadKeyBytes>& key(Direction dir) const;

private:
    std::array<std::uint8_t, kAeadKeyBytes> k_c2s_{};
    std::array<std::uint8_t, kAeadKeyBytes> k_s2c_{};
    std::uint64_t seq_c2s_ = 0;
    std::uint64_t seq_s2c_ = 0;
    bool ok_ = false;
};

}  // namespace meridian::clientnet

#endif  // MERIDIAN_CLIENTNET_WORLD_SESSION_H
