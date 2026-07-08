// SPDX-License-Identifier: Apache-2.0
//
// worldd — IF-3 session establishment: grant validation + AEAD session crypto
// (issue #84). See world_dispatch.h for the process scaffold this builds on.
//
// CLEAN-ROOM: designed from the server SAD only — §5.2 (IF-2 framing + the
// per-session AEAD channel), §5.3 (IF-3 session handoff: the session_grant
// table, single-use atomic consume, 30 s expiry, bound to {account, realm,
// client_build}), §4.1 (session_grant DDL), §4.2 (characters DB), and the
// OpenSSL EVP public API docs. No GPL source (CMaNGOS / TrinityCore or
// otherwise) was consulted. See CONTRIBUTING.md.
//
// TWO concerns live here, both the IF-3 half of the WorldHello handshake:
//
//   1. GRANT VALIDATION + SINGLE-USE CONSUME (SAD §5.3, §3.1). On WorldHello we
//      look the grant_id up in the auth DB `session_grant` table and consume it
//      with ONE atomic UPDATE (SET consumed_at WHERE grant_id=? AND consumed_at
//      IS NULL AND expires_at > now). affected_rows == 1 is the accept; a
//      replay or an expired/unknown grant affects 0 rows and is rejected. This
//      is the single-use guarantee — it lives in the DB's row lock, not in app
//      logic, so two racing WorldHellos cannot both win. Mirrors exactly how
//      authd/the authd integration test consume the same row (same schema, same
//      decimal-string binding for the BIGINT UNSIGNED columns).
//
//   2. AEAD SESSION (SAD §5.2). After a valid grant, both ends key a
//      ChaCha20-Poly1305 channel off the 32-byte session_key. Per SAD §5.2:
//        k_c2s, k_s2c = HKDF-SHA256(session_key, "meridian-world-v1", direction)
//        payloads ChaCha20-Poly1305, nonce = direction ∥ 64-bit sequence counter
//      The WorldSession object below owns the two derived keys + the two
//      monotonic counters and exposes seal()/open() so the post-handshake frame
//      codec (#82/#83's encode/decode seam) can wrap/unwrap IF-2 payloads. The
//      nonce is NEVER reused under a key: each direction has its own key and its
//      own strictly-incrementing 64-bit counter, and seal() refuses to wrap once
//      a counter would wrap around (2^64 frames — unreachable in practice, but a
//      hard stop rather than a silent nonce reuse).
//
// M0 NOTE: at M0 worldd is still the client-facing process (the gateway split is
// M2, SAD §2.2), so worldd — not gatewayd — consumes the grant and holds the
// session_key here. The seam is identical; only the owning process moves.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"

#include "world_generated.h"

namespace meridian::worldd {

using net::Bytes;

// ---------------------------------------------------------------------------
// AEAD session (SAD §5.2)
// ---------------------------------------------------------------------------

// ChaCha20-Poly1305 sizes (RFC 8439). The key is 256-bit; the nonce is 96-bit;
// the auth tag is 128-bit. These are fixed by the cipher and by SAD §5.2.
inline constexpr std::size_t kAeadKeyBytes = 32;   // ChaCha20 key
inline constexpr std::size_t kAeadNonceBytes = 12; // 96-bit nonce
inline constexpr std::size_t kAeadTagBytes = 16;   // Poly1305 tag

// Wire direction. Names the HKDF `info`/`direction` label and the top byte of
// the nonce so the two directions can never collide on a nonce even though they
// share the source session_key (they derive DIFFERENT keys, and additionally
// tag the nonce). c2s = client→server, s2c = server→client.
enum class Direction : std::uint8_t {
    kClientToServer = 0,
    kServerToClient = 1,
};

// A per-session AEAD channel keyed off a 32-byte session_key (SAD §5.2).
//
// Key derivation (matches the SAD formula exactly):
//   k_c2s = HKDF-SHA256(ikm=session_key, salt="", info="meridian-world-v1\x00")
//   k_s2c = HKDF-SHA256(ikm=session_key, salt="", info="meridian-world-v1\x01")
// where the trailing direction byte (0/1) is the §5.2 `direction` selector, so
// the two directions get independent 32-byte keys.
//
// Nonce scheme (SAD §5.2 "nonce = direction ∥ 64-bit sequence counter"):
//   the 96-bit ChaCha20 nonce is  [ direction : 1 byte ][ zero : 3 bytes ]
//                                 [ seq counter : 8 bytes, big-endian ]
//   Each direction has its own counter starting at 0, incremented once per
//   sealed frame. A given (key, nonce) pair is therefore used exactly once:
//   the counter is monotonic within a direction, and the direction byte + the
//   distinct per-direction key separate the two streams. seal()/open() advance
//   the counter; a caller must seal/open frames in order (the IF-2 u64 seq in
//   the frame header is the client-visible echo of this counter — the codec
//   keeps them in lockstep).
//
// Not thread-safe: one WorldSession is driven by one connection's IO worker,
// which already owns that socket (SAD §6.1). No sharing is required.
class WorldSession {
public:
    // Derive both direction keys from `session_key` (must be exactly 32 bytes).
    // Throws net::TlsError on a wrong-length key or an OpenSSL HKDF failure —
    // a session must never proceed with mis-derived keys.
    explicit WorldSession(const Bytes& session_key);

    // Seal `plaintext` for transmission in `dir` (the sender's own direction).
    // Returns nonce_seq ∥ ciphertext ∥ tag is NOT the shape — instead the caller
    // gets back just ciphertext‖tag and the sequence number it was sealed under
    // (via out_seq), because the IF-2 frame header already carries the u64 seq on
    // the wire, so the nonce is reconstructible by the peer from (direction,
    // frame.seq) without spending extra bytes. `aad` binds additional
    // authenticated data (e.g. the opcode+seq header) into the tag; pass empty
    // for none. Advances this direction's counter by one. Throws net::TlsError on
    // counter exhaustion or an OpenSSL failure.
    Bytes seal(Direction dir, const Bytes& plaintext, const Bytes& aad,
               std::uint64_t& out_seq);

    // Open a frame received on `dir` (the peer's direction) that was sealed under
    // sequence `seq` with the given `aad`. Returns the plaintext on a valid tag,
    // or std::nullopt if authentication fails (tampered ciphertext / wrong seq /
    // wrong aad) — a failed open is a protocol error the caller treats as a
    // disconnect, never a silent accept. Does NOT advance a counter (the receiver
    // trusts the wire seq for the nonce; replay/ordering enforcement is the
    // codec's concern above this layer).
    std::optional<Bytes> open(Direction dir, const Bytes& ciphertext_and_tag,
                              std::uint64_t seq, const Bytes& aad);

    // The next sequence number seal() will use for `dir` (test/diagnostic).
    std::uint64_t next_seq(Direction dir) const;

    // The derived 32-byte key for a direction (test/diagnostic; the key never
    // leaves the process in production — SAD §5.3).
    const std::array<std::uint8_t, kAeadKeyBytes>& key(Direction dir) const;

private:
    std::array<std::uint8_t, kAeadKeyBytes> k_c2s_{};
    std::array<std::uint8_t, kAeadKeyBytes> k_s2c_{};
    std::uint64_t seq_c2s_ = 0;
    std::uint64_t seq_s2c_ = 0;
};

// ---------------------------------------------------------------------------
// Grant validation + single-use consume (SAD §5.3, §3.1)
// ---------------------------------------------------------------------------

// Why a WorldHello was rejected. Maps 1:1 to a net::DisconnectReason for the
// wire, but is more specific for logging + testing (the wire reason for every
// grant failure is GRANT_INVALID, per world.fbs, so we do not leak WHICH check
// failed to the client — an unknown/expired/consumed grant are indistinguishable
// on the wire, denying an attacker an oracle).
enum class GrantReject {
    kUnknown,          // no row for this grant_id
    kExpired,          // expires_at <= now
    kAlreadyConsumed,  // consumed_at already set (a replay)
    kWrongRealm,       // grant bound to a different realm than this worldd serves
    kDbError,          // the DB call itself failed
};

// The successful result of consuming a grant: the account + realm it was bound
// to and the 32-byte session_key (for deriving the AEAD channel).
struct GrantConsumed {
    std::uint64_t account_id = 0;
    std::uint32_t realm_id = 0;
    Bytes session_key;  // 32 bytes (SAD §4.1 BINARY(32))
};

// Look up `grant_id` in `session_grant`, validate it, and ATOMICALLY consume it
// (single-use). `expected_realm_id` is the realm this worldd serves — a grant
// for another realm is rejected (SAD §5.3 "bound to {account, realm,
// client_build}"). Returns the {account, realm, session_key} on success, or a
// GrantReject on any failure.
//
// The consume is ONE UPDATE, and it is the single-use guarantee:
//   UPDATE session_grant SET consumed_at = UTC_TIMESTAMP()
//     WHERE grant_id = ? AND consumed_at IS NULL AND expires_at > UTC_TIMESTAMP();
// affected_rows == 1 => this caller won the row (accept); == 0 => the grant was
// already consumed, expired, or never existed (reject). We SELECT afterward (the
// same connection, still cheap) only to fetch account_id/realm_id/session_key
// for the winning row and to distinguish the reject reasons for logging. The
// SELECT is NOT the guard — the UPDATE's row lock is (two racing consumers, only
// one gets affected_rows==1). grant_id/account_id bind as decimal strings to
// survive the full u64 range on the BIGINT UNSIGNED columns (matching how authd
// wrote them).
std::optional<GrantConsumed> validate_and_consume_grant(
    db::Connection& db, std::uint64_t grant_id, std::uint32_t expected_realm_id,
    GrantReject& out_reject);

// ---------------------------------------------------------------------------
// Enter-world: server-authoritative character load (D-35 / #341)
// ---------------------------------------------------------------------------

// A real, server-persisted character loaded on ENTER_WORLD. D-11 scopes M0 to
// "name + class selection over one placeholder character model" — so this is the
// name + class of the OWNED character, plus the guid/level the client needs to
// spawn its own entity. There is NO fabrication: this is only ever populated
// from an actual `character` row owned by the session's account.
struct LoadedCharacter {
    std::uint64_t char_guid = 0;  // character.id
    std::string name;
    std::uint8_t class_id = 0;
    std::uint16_t level = 1;
};

// Load character `character_id` IFF it is owned by `account_id`
// (`SELECT id, name, class, level FROM character WHERE id=? AND account_id=?`).
// Server is the source of truth: returns std::nullopt when no such owned row
// exists (a bad/nonexistent id, or a character owned by ANOTHER account) — the
// caller REJECTS enter-world rather than synthesising a placeholder. `char_db`
// is a live connection (the caller checks non-null first). Throws db::DbError on
// a DB fault, which the caller maps to ENTER_WORLD `INTERNAL`.
std::optional<LoadedCharacter> load_owned_character(db::Connection& char_db,
                                                    std::uint64_t account_id,
                                                    std::uint64_t character_id);

}  // namespace meridian::worldd
