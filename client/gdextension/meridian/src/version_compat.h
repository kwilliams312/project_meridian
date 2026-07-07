// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free SCHEMA/PROTOCOL VERSION COMPAT core (issue #98).
//
// The client SAD requires (client-sad.md §5.1): "version mismatch at either hop →
// 'client out of date', never UB"; and the server SAD compat rule (server-sad.md
// §5.2): "proto_ver + content hash at handshake; version mismatch = reject with a
// human-readable reason; content-hash mismatch = warning (M0–M1) / reject (M1+ test
// realm)." This unit is the ONE place that rule is defined, so the IF-1 login core
// (login_core.*, ClientHello/ServerHello.proto_ver) and — when worldd populates it —
// the IF-2 world handshake (HandshakeOk.content_hash) compare versions identically
// instead of open-coding an ad-hoc `!=`.
//
// ENGINE-FREE + DEPENDENCY-FREE (Client SAD §9.2): plain C++17, no Godot, no
// FlatBuffers, no OpenSSL, no sockets. It takes already-decoded scalars/bytes so the
// caller (which owns the wire decode) hands it plain values, and it is unit-tested
// as pure logic (version_compat_test.cpp, ctest `version-compat`, MERIDIAN_CLIENT_TESTS).
//
// ── THE M0 COMPARE RULE (exact-match proto, advisory content hash) ─────────────
//   • proto_ver — EXACT match. FlatBuffers evolves additively WITHIN a major; majors
//     are incompatible (auth.fbs / world.fbs headers). At M0 the client cannot tell a
//     newer server from an older one (there is no min-supported floor on the wire yet),
//     so ANY proto_ver difference — client older OR newer — is kClientOutOfDate and is
//     ALWAYS blocking: the client must update. (Documented handling for the
//     client-newer case: same as client-older — prompt to update, never proceed.)
//   • content_hash — the aggregate content hash the realm advertises (BLAKE3-256, IF-4).
//     Compared only when BOTH sides present a well-formed hash. A difference is
//     kContentMismatch and is blocking ONLY under a hard-fail policy (M1+ test realm);
//     at M0–M1 it is a non-blocking WARNING (proceed). An absent hash on either side
//     means "no content check performed" (compatible). A present-but-wrong-width hash is
//     kMalformed and is handled the SAME safe way as a mismatch (warn at M0, never UB).

#ifndef MERIDIAN_VERSION_COMPAT_H
#define MERIDIAN_VERSION_COMPAT_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace meridian::compat {

// A raw content-hash (BLAKE3-256 = 32 bytes) or empty when none is advertised.
using HashBytes = std::vector<std::uint8_t>;

// One side's advertised schema/protocol version. content_hash may be empty: IF-1
// (ClientHello/ServerHello) carries only proto_ver, and at M0 worldd's HandshakeOk
// leaves content_hash empty — both cases mean "no content hash to compare".
struct SchemaVersion {
    std::uint16_t proto_ver = 0;
    HashBytes content_hash;  // empty == not advertised
};

// Milestone-gated severity for the content-hash arm of the rule. proto_ver is ALWAYS
// exact-match/blocking and is not affected by policy.
struct CompatPolicy {
    // Content-hash mismatch severity: false (M0–M1) = warn + proceed; true (M1+ test
    // realm, server-sad.md §5.2) = reject. Also gates the kMalformed severity.
    bool content_hash_hard_fail = false;
    // Expected content-hash width in bytes (BLAKE3-256 = 32). A present hash of a
    // different width is kMalformed. 0 disables the width check.
    std::size_t content_hash_len = 32;
};

// The outcome category of a compare.
enum class CompatVerdict : std::uint8_t {
    kCompatible = 0,       // proceed normally (happy path — no behavior change)
    kClientOutOfDate = 1,  // proto_ver mismatch — the terminal "client out of date" case
    kContentMismatch = 2,  // content hashes differ — blocking per policy
    kMalformed = 3,        // a content hash was present but ill-formed — safe-handled
};

const char* to_string(CompatVerdict v);

// The result of check_compat: a category, whether the caller MUST route to the
// out-of-date UX (blocking) rather than proceed, and a short human-readable note.
struct CompatResult {
    CompatVerdict verdict = CompatVerdict::kCompatible;
    bool blocking = false;  // true => do NOT proceed; surface "client out of date"
    const char* detail = "versions compatible";  // static string (logs / fallback UX)

    // Convenience: the happy path is "not blocking".
    bool ok() const { return !blocking; }
};

// Compare the client's `local` schema version against the server-advertised `remote`
// under `policy` (default = M0: exact-match proto, advisory content hash). Pure; never
// throws. See the header comment for the exact rule.
CompatResult check_compat(const SchemaVersion& local, const SchemaVersion& remote,
                          const CompatPolicy& policy = {});

}  // namespace meridian::compat

#endif  // MERIDIAN_VERSION_COMPAT_H
