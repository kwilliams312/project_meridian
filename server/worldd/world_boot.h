// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB boot: content manifest check + content hash (IF-4; issue
// #89). See world_dispatch.h / world_session.h for the process scaffold and the
// IF-3 session path this boot step runs *before*.
//
// CLEAN-ROOM: designed from the server SAD only — §4.3 (world DB is a read-only
// mcc artifact; the boot handshake reads `manifest`, refuses to start on a
// schema-version mismatch, and logs the content hash), §5.4.3 (content-hash tie:
// warning M0–M1 / hard fail on the test realm from M1), the IF-4 row in §2's
// interface table (worldd *consumes* the compiled world data + manifest), and
// the world DDL `schema/sql/world/00_manifest.sql` (the `world_manifest` table
// mcc fills and worldd reads verbatim). No GPL source consulted (CONTRIBUTING).
//
// WHY a boot step at all (SAD §4.3 / §5.4.3): worldd serves a *specific compile*
// of the content. If the world DB is a version worldd cannot serve (schema drift
// between the mcc emitter and this binary), or the manifest is missing/truncated
// (a partial or failed nightly load), the server must NOT silently serve garbage
// — it fails fast. If the loaded content hash disagrees with an operator-pinned
// expected hash (the realm's "this is the compile we deployed" tie), that is an
// advisory warning at M0–M1 and becomes a hard fail on the test realm from M1.
//
// SCOPE (#89): worldd's boot-time READ + VERIFY of the manifest mcc writes. The
// PRODUCER side (mcc `emit-sql` populating `world_manifest`) is a separate Tools
// story — the emit-sql/bake stages are declared (tools/mcc stages.h) but not yet
// implemented. IF-4 CONTRACT: the row shape below is exactly `world_manifest`'s
// columns; mcc MUST populate them. Documented as a follow-up in the PR.
//
// The verify logic (verify_world_manifest) is a PURE function over already-read
// rows — no DB, no I/O — so it is unit-testable DB-free. The DB read
// (read_world_manifest) is the thin MariaDB SELECT that feeds it.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "meridian/db/connection.h"

namespace meridian::worldd {

// The content-schema major version THIS worldd binary understands. Bumped in
// lockstep with a breaking change to the world DDL (`schema/sql/world/*.sql`).
// The world DDL is currently v1 (schema/sql/world/README.md "World DB DDL v1"),
// so a world DB whose manifest reports any other `schema_version` is one this
// binary cannot serve -> hard fail (SAD §4.3 "refuses to start on schema-version
// mismatch"). Keeping this a named constant makes the DDL bump a one-line change.
inline constexpr std::uint32_t kSupportedContentSchemaVersion = 1;

// The fixed width of a BLAKE3 content hash rendered as lowercase hex
// (32 bytes -> 64 hex chars). The `world_manifest.content_hash` column is
// CHAR(64); a value that is not exactly 64 hex chars is malformed (a truncated
// or non-hex hash means a corrupt / partial manifest row).
inline constexpr std::size_t kContentHashHexLen = 64;

// One row of `world_manifest` (schema/sql/world/00_manifest.sql). One row per
// content pack merged into the world DB; worldd reads every row at boot. Field
// order + names mirror the DDL columns exactly (the IF-4 contract): mcc writes
// them, worldd reads them.
struct ManifestRow {
    std::string   pack_namespace;   // pack.namespace (PK); owns an IF-9 ID band
    std::string   pack_version;     // pack.version (semver text)
    std::uint32_t id_band = 0;      // IF-9 numeric band base for this pack
    std::string   content_hash;     // BLAKE3 of the pack source tree (64 hex)
    std::uint32_t schema_version = 0;  // content schema major (must match ours)
    std::string   mcc_version;      // compiler version that produced the row
    std::string   built_at;         // build timestamp (DATETIME text)
};

// How the boot verify came out. `Ok` and `SoftWarn` both let worldd boot;
// `SoftWarn` additionally logs a prominent warning (the advisory content-hash
// tie at M0–M1). Every other verdict is a HARD failure: worldd refuses to boot.
enum class BootVerdict {
    kOk,                    // manifest present, well-formed, schema matches (and
                            // the expected hash matched if one was pinned)
    kSoftWarn,             // bootable, but the pinned expected hash disagreed —
                            // advisory only at M0–M1 (SAD §5.4.3)
    kMissingManifest,      // no manifest rows at all (empty / un-loaded world DB)
    kMalformedManifest,    // a row is structurally invalid (bad hash width, empty
                           // required field) — a truncated / partial load
    kSchemaMismatch,       // a row's schema_version != kSupportedContentSchemaVersion
};

// The outcome of the boot check: the verdict, a human-readable reason (for the
// log / the fatal message), and the resolved content identity worldd loaded
// (the primary pack's namespace/version/hash) when one is available. `hard_fail`
// is the single "should worldd refuse to boot?" bit the caller acts on.
struct BootReport {
    BootVerdict verdict = BootVerdict::kMissingManifest;
    bool        hard_fail = true;   // true unless verdict is kOk or kSoftWarn
    std::string reason;             // one-line explanation for logs

    // Resolved content identity (from the primary pack — the "core" namespace if
    // present, else the first row). Empty on a missing/malformed manifest.
    std::string content_hash;       // the loaded content hash (for HandshakeOk)
    std::string content_version;    // "<namespace>@<pack_version>" of the primary
    std::uint32_t schema_version = 0;
    std::size_t pack_count = 0;     // number of packs in the manifest
};

// PURE verify (no DB, no I/O — unit-testable). Validate the manifest rows read
// from the world DB against this binary's contract:
//   * at least one row present            -> else kMissingManifest (hard)
//   * every row well-formed (namespace non-empty; content_hash is exactly 64
//     lowercase-hex chars)                -> else kMalformedManifest (hard)
//   * every row's schema_version matches  -> else kSchemaMismatch (hard)
//   * if `expected_content_hash` is set, the resolved (primary) content hash
//     equals it                           -> else kSoftWarn (advisory, bootable)
//
// `expected_content_hash` is the operator-pinned "this is the compile we
// deployed" tie (SAD §5.4.3). Empty (nullopt/"") means "no pin" -> the hash tie
// is not checked and a well-formed manifest is kOk. When set it must itself be
// 64 hex chars to be a meaningful pin; a malformed pin is ignored (treated as no
// pin) rather than failing boot, since it is operator config, not content.
//
// The "primary" pack is the row whose namespace is "core" if present, otherwise
// the first row — its identity is what worldd logs and propagates in HandshakeOk.
BootReport verify_world_manifest(
    const std::vector<ManifestRow>& rows,
    const std::optional<std::string>& expected_content_hash = std::nullopt);

// Read every `world_manifest` row from the connected world DB. Thin SELECT over
// meridian::db::Connection (parameterless; the whole table is read). Throws
// db::DbError on a query failure (e.g. the table does not exist -> an un-loaded
// world DB) — the caller (boot_world_db) turns that into a hard fail.
std::vector<ManifestRow> read_world_manifest(db::Connection& world_db);

// Full boot-time content check: read the manifest from `world_db`, verify it,
// and LOG the outcome prominently (the loaded content version + hash on success;
// the reason on warn/fail). Returns the BootReport; the caller decides whether to
// refuse to boot (report.hard_fail). A DB/read failure is captured as a
// kMissingManifest hard fail with the DB error in `reason` (never throws).
//
// `expected_content_hash` is the optional operator pin (see verify_world_manifest).
BootReport boot_world_db(
    db::Connection& world_db,
    const std::optional<std::string>& expected_content_hash = std::nullopt);

// Human-readable name for a verdict (logs / test diagnostics).
const char* boot_verdict_name(BootVerdict v);

}  // namespace meridian::worldd
