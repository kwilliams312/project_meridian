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
    std::uint32_t compatibility_version = 1;  // pack CONTRACT version (#698); the
                                    // boot compat gate compares this against the
                                    // realm's persisted state. Absent -> 1.
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
    kCompatBreaking,       // the loaded pack's compatibility_version differs from
                           // the realm's persisted state — a BREAKING pack change
                           // that refuses to boot until an operator migrates (#698)
};

// The outcome of the boot check: the verdict, a human-readable reason (for the
// log / the fatal message), and the resolved content identity worldd loaded
// (the primary pack's namespace/version/hash) when one is available. `hard_fail`
// is the single "should worldd refuse to boot?" bit the caller acts on.
struct BootReport {
    BootVerdict verdict = BootVerdict::kMissingManifest;
    bool        hard_fail = true;   // true unless verdict is kOk, kSoftWarn, or a
                                    // degraded (empty-but-not-required) world DB
    bool        degraded = false;   // an empty/missing world DB that boot policy
                                    // let through WITHOUT content (issue #485):
                                    // kMissingManifest + require_content off ->
                                    // serve content-less and self-heal on seed.
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

// ---------------------------------------------------------------------------
// Boot-time compatibility / migration gate (SP2.8, issue #698)
// ---------------------------------------------------------------------------
//
// The realm's PERSISTED compatibility state — the compatibility_version the realm
// last successfully booted with, PER pack namespace. Lives in the CHARACTERS DB
// (realm_content_state, migration 0004) because it must survive a content reload:
// the world DB is a read-only mcc artifact replaced wholesale on every build, so
// persisting the marker there would reset it to the loaded value and make the gate
// a no-op. One row per pack namespace, mirroring world_manifest's granularity.
struct RealmCompatRow {
    std::string   pack_namespace;             // pack.namespace (PK)
    std::uint32_t compatibility_version = 1;  // the version the realm booted with
    std::string   content_hash;               // the pack hash last booted (context)
    std::string   pack_version;               // the semver last booted (context)
};

// The outcome of the compat gate over already-read rows (PURE — see below).
struct CompatGateResult {
    bool breaking = false;   // a breaking mismatch was found -> refuse boot
    std::string reason;      // actionable report naming each broken pack (mirrors
                             // `mcc diff`'s vocabulary); empty when not breaking
    // The packs to (re)record into realm_content_state on a SAFE boot: every
    // loaded pack that is fresh (no persisted row) or compatible (loaded
    // compatibility_version equals the persisted one). Empty when `breaking`
    // (worldd records nothing on a refusal — the operator migration advances the
    // persisted version after migrating character data).
    std::vector<ManifestRow> to_record;
};

// PURE compat gate (no DB, no I/O — unit-testable). Compare the LOADED packs
// (world_manifest rows just read from the world DB) against the realm's PERSISTED
// state, per pack namespace, per the umbrella §6.5 contract:
//   * a pack with no persisted row            -> FRESH: record it, boot freely;
//   * loaded compatibility_version == persisted -> COMPATIBLE (identical, or an
//     additive change that never bumps the version): record (refresh hash), boot;
//   * loaded compatibility_version != persisted -> BREAKING: a removed/renumbered
//     id crossed the compatibility boundary (a higher loaded value) or the pack
//     was rolled back below one (a lower value) -> refuse to boot;
//   * a persisted pack absent from the loaded set -> BREAKING: the whole pack (all
//     its ids) was removed from the content the realm booted with.
// A breaking result names every offending pack in `reason` and records nothing.
CompatGateResult verify_compat_gate(const std::vector<ManifestRow>& loaded,
                                    const std::vector<RealmCompatRow>& persisted);

// Read the realm's persisted compat state from the connected characters DB (every
// realm_content_state row). Thin SELECT; throws db::DbError on a query failure
// (e.g. the table does not exist -> a characters DB not migrated to 0004).
std::vector<RealmCompatRow> read_realm_compat_state(db::Connection& char_db);

// Record (UPSERT) the given loaded packs into the characters DB
// realm_content_state — "worldd records the pack compatibility_version it booted
// with" (#698). Called only for the SAFE packs the gate returned in `to_record`.
// One INSERT ... ON DUPLICATE KEY UPDATE per pack. Throws db::DbError on failure.
void record_realm_compat_state(db::Connection& char_db,
                               const std::vector<ManifestRow>& rows);

// Full boot-time content check: read the manifest from `world_db`, verify it,
// and LOG the outcome prominently (the loaded content version + hash on success;
// the reason on warn/fail/degrade). Returns the BootReport; the caller decides
// whether to refuse to boot (report.hard_fail). A world_db read failure is
// captured as a kMissingManifest report with the DB error in `reason` (the
// world-DB path never throws). A CHARACTERS-DB failure during the compat gate
// (below) does propagate as db::DbError — the caller already treats that as a
// hard boot failure.
//
// `expected_content_hash` is the optional operator pin (see verify_world_manifest).
//
// `require_content` is the boot POLICY for an EMPTY / missing world DB (issue
// #485). Default false = DEGRADE: a configured-but-empty world DB
// (kMissingManifest — no manifest rows, or the manifest table not yet created by
// the seed) is NOT fatal; worldd boots WITHOUT content (report.degraded = true,
// hard_fail = false) exactly like the WORLDDB-unset path, so a client still
// connects and the realm self-heals once the content-seed lands. This kills the
// CrashLoopBackOff when worldd restarts before the seed Job finishes. Set true
// (env MERIDIAN_WORLDDB_REQUIRE_CONTENT=1) to keep the strict fail-fast: an empty
// world DB then hard-fails as before. INTEGRITY faults (kMalformedManifest,
// kSchemaMismatch — corrupt or unserveable content) ALWAYS hard-fail regardless
// of this flag; only the empty/missing case degrades.
//
// `char_db` is the CHARACTERS DB connection the boot compat gate (#698) uses.
// When non-null AND the manifest verdict is bootable (kOk / kSoftWarn), the gate
// runs: it reads the realm's persisted compatibility state (realm_content_state)
// and compares it against the loaded packs. A BREAKING mismatch flips the report
// to kCompatBreaking + hard_fail with an actionable reason (mirroring `mcc diff`);
// a fresh/compatible boot RECORDS the loaded state (worldd "records the pack
// compatibility_version it booted with"). Null (no characters DB wired) SKIPS the
// gate — the daemon still boots, exactly like the auth-DB-less grant path. The
// gate never runs on a degraded / missing / malformed manifest (no content to
// gate).
BootReport boot_world_db(
    db::Connection& world_db,
    const std::optional<std::string>& expected_content_hash = std::nullopt,
    bool require_content = false,
    db::Connection* char_db = nullptr);

// Human-readable name for a verdict (logs / test diagnostics).
const char* boot_verdict_name(BootVerdict v);

}  // namespace meridian::worldd
