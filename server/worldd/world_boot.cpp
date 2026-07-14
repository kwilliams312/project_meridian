// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB boot implementation (IF-4; issue #89). See world_boot.h for
// the design + the SAD/DDL citations. Clean-room from the SAD + the world DDL;
// no GPL source consulted (CONTRIBUTING.md).

#include "world_boot.h"

#include "meridian/core/log.hpp"

#include <cctype>
#include <map>
#include <set>

namespace meridian::worldd {
namespace {

constexpr const char* kCat = "worldd";

// The manifest namespace worldd treats as the primary content pack — its
// identity is logged + propagated in HandshakeOk. "core" is the base pack per
// the IF-9 namespace convention (schema/sql/world/00_manifest.sql: pack.namespace
// owns an ID band; "core" is the baseline band). Absent -> use the first row.
constexpr const char* kPrimaryNamespace = "core";

// A well-formed content hash is exactly kContentHashHexLen lowercase-hex chars.
// mcc renders BLAKE3 as lowercase hex (world_manifest.content_hash CHAR(64)); a
// wrong width or a non-hex char means a truncated / corrupt manifest row.
bool is_well_formed_hash(const std::string& h) {
    if (h.size() != kContentHashHexLen) return false;
    for (const char c : h) {
        const bool lower_hex =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) return false;
    }
    return true;
}

// Parse a decimal cell (world_manifest numeric columns come back as text via the
// M0 Cell API). Missing / non-numeric -> 0 (a 0 schema_version is itself a
// mismatch against v1, so a garbage cell fails loudly downstream).
std::uint32_t cell_u32(const db::Cell& c) {
    if (!c.has_value()) return 0;
    std::uint64_t v = 0;
    for (const char ch : *c) {
        if (ch < '0' || ch > '9') break;
        v = v * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return static_cast<std::uint32_t>(v);
}

std::string cell_str(const db::Cell& c) { return c.has_value() ? *c : std::string{}; }

// Pick the primary row: the "core" namespace if present, else the first row.
const ManifestRow& primary_row(const std::vector<ManifestRow>& rows) {
    for (const ManifestRow& r : rows) {
        if (r.pack_namespace == kPrimaryNamespace) return r;
    }
    return rows.front();
}

}  // namespace

const char* boot_verdict_name(BootVerdict v) {
    switch (v) {
        case BootVerdict::kOk:               return "ok";
        case BootVerdict::kSoftWarn:         return "soft-warn";
        case BootVerdict::kMissingManifest:  return "missing-manifest";
        case BootVerdict::kMalformedManifest: return "malformed-manifest";
        case BootVerdict::kSchemaMismatch:   return "schema-mismatch";
        case BootVerdict::kCompatBreaking:   return "compat-breaking";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// PURE verify (no DB)
// ---------------------------------------------------------------------------

BootReport verify_world_manifest(
    const std::vector<ManifestRow>& rows,
    const std::optional<std::string>& expected_content_hash) {
    BootReport rep;
    rep.pack_count = rows.size();

    // (1) Present at all? An empty world DB (nightly load never ran, or ran and
    // failed before the manifest write) has zero rows -> hard fail (SAD §4.3:
    // worldd reads `manifest` at boot; nothing to read = refuse).
    if (rows.empty()) {
        rep.verdict = BootVerdict::kMissingManifest;
        rep.hard_fail = true;
        rep.reason = "world_manifest is empty (no content packs) — world DB not "
                     "loaded or load truncated";
        return rep;
    }

    // (2) Well-formed? Every row must have a non-empty namespace and a 64-hex
    // content hash. A malformed row is a truncated / partial load (SAD §4.3).
    for (const ManifestRow& r : rows) {
        if (r.pack_namespace.empty()) {
            rep.verdict = BootVerdict::kMalformedManifest;
            rep.hard_fail = true;
            rep.reason = "world_manifest row has an empty pack_namespace "
                         "(corrupt manifest)";
            return rep;
        }
        if (!is_well_formed_hash(r.content_hash)) {
            rep.verdict = BootVerdict::kMalformedManifest;
            rep.hard_fail = true;
            rep.reason = "world_manifest pack '" + r.pack_namespace +
                         "' has a malformed content_hash (expected 64 lowercase-hex "
                         "chars) — truncated / corrupt content load";
            return rep;
        }
    }

    // (3) Schema this binary can serve? Every pack must report the content-schema
    // major this worldd understands (SAD §4.3 "refuses to start on schema-version
    // mismatch"). A single mismatched pack fails the whole boot — the world DB is
    // a version worldd cannot serve.
    for (const ManifestRow& r : rows) {
        if (r.schema_version != kSupportedContentSchemaVersion) {
            rep.verdict = BootVerdict::kSchemaMismatch;
            rep.hard_fail = true;
            rep.reason =
                "world_manifest pack '" + r.pack_namespace + "' reports schema_version " +
                std::to_string(r.schema_version) + " but this worldd serves content-schema v" +
                std::to_string(kSupportedContentSchemaVersion) +
                " — rebuild the world DB with a matching mcc, or upgrade worldd";
            return rep;
        }
    }

    // The manifest is present, well-formed, and serveable. Resolve the primary
    // pack's identity for the log + HandshakeOk.
    const ManifestRow& primary = primary_row(rows);
    rep.content_hash = primary.content_hash;
    rep.content_version = primary.pack_namespace + "@" + primary.pack_version;
    rep.schema_version = primary.schema_version;

    // (4) Optional operator-pinned hash tie (SAD §5.4.3). Only when a pin is set
    // AND is itself well-formed — a malformed pin is operator config noise, not a
    // content fault, so it is ignored (no-pin) rather than failing boot. A real
    // disagreement is an advisory SoftWarn at M0–M1 (bootable); this is the seam
    // where it becomes a hard fail on the test realm from M1.
    if (expected_content_hash && is_well_formed_hash(*expected_content_hash) &&
        *expected_content_hash != rep.content_hash) {
        rep.verdict = BootVerdict::kSoftWarn;
        rep.hard_fail = false;
        rep.reason = "loaded content hash " + rep.content_hash +
                     " does not match the operator-pinned expected hash " +
                     *expected_content_hash +
                     " — advisory at M0–M1 (SAD §5.4.3), hard fail on test realm from M1";
        return rep;
    }

    rep.verdict = BootVerdict::kOk;
    rep.hard_fail = false;
    rep.reason = "world manifest ok";
    return rep;
}

// ---------------------------------------------------------------------------
// DB read
// ---------------------------------------------------------------------------

std::vector<ManifestRow> read_world_manifest(db::Connection& world_db) {
    // Read every pack row. Deterministic order (by namespace) so logs + the
    // primary-pick tie-break are stable across runs. Column order is fixed by the
    // SELECT, not by the DB's declaration order.
    db::Result res = world_db.execute(
        "SELECT pack_namespace, pack_version, id_band, content_hash, "
        "schema_version, compatibility_version, mcc_version, built_at "
        "FROM world_manifest ORDER BY pack_namespace");

    std::vector<ManifestRow> rows;
    rows.reserve(res.rows.size());
    for (const db::Row& r : res.rows) {
        ManifestRow m;
        m.pack_namespace        = cell_str(r[0]);
        m.pack_version          = cell_str(r[1]);
        m.id_band               = cell_u32(r[2]);
        m.content_hash          = cell_str(r[3]);
        m.schema_version        = cell_u32(r[4]);
        m.compatibility_version = cell_u32(r[5]);
        m.mcc_version           = cell_str(r[6]);
        m.built_at              = cell_str(r[7]);
        rows.push_back(std::move(m));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Boot-time compatibility / migration gate (#698)
// ---------------------------------------------------------------------------

CompatGateResult verify_compat_gate(const std::vector<ManifestRow>& loaded,
                                    const std::vector<RealmCompatRow>& persisted) {
    CompatGateResult result;

    // Index the persisted state by namespace for O(1) per-pack lookup. std::map
    // keeps the "removed pack" scan (below) deterministic (sorted by namespace).
    std::map<std::string, const RealmCompatRow*> persisted_by_ns;
    for (const RealmCompatRow& p : persisted) persisted_by_ns[p.pack_namespace] = &p;

    // Track which persisted packs the loaded set covers, to flag removed packs.
    std::set<std::string> loaded_ns;

    std::vector<std::string> findings;  // one actionable line per breaking pack

    for (const ManifestRow& l : loaded) {
        loaded_ns.insert(l.pack_namespace);
        const auto it = persisted_by_ns.find(l.pack_namespace);
        if (it == persisted_by_ns.end()) {
            // FRESH pack: the realm has never booted it -> record + boot freely.
            result.to_record.push_back(l);
            continue;
        }
        const std::uint32_t was = it->second->compatibility_version;
        const std::uint32_t now = l.compatibility_version;
        if (now == was) {
            // COMPATIBLE: identical, or an additive change that never bumps the
            // version. Record (refreshes the stored hash/version) + boot.
            result.to_record.push_back(l);
            continue;
        }
        // BREAKING: the loaded compatibility_version differs from the one the
        // realm booted with. Name it, mirroring `mcc diff`'s report vocabulary.
        std::string line = "pack '" + l.pack_namespace + "': compatibility_version " +
                           std::to_string(was) + " (realm last booted) -> " +
                           std::to_string(now) + " (loaded pack) — ";
        line += (now > was)
                    ? "a removed/renumbered id crossed the compatibility boundary"
                    : "the pack was rolled back below a compatibility boundary";
        findings.push_back(std::move(line));
    }

    // A persisted pack the loaded content no longer provides: the whole pack (all
    // of its ids) vanished from the content the realm booted with — breaking.
    for (const auto& [ns, row] : persisted_by_ns) {
        if (loaded_ns.count(ns)) continue;
        findings.push_back("pack '" + ns + "': present in the realm state "
                           "(compatibility_version " +
                           std::to_string(row->compatibility_version) +
                           ") but ABSENT from the loaded content — the entire pack "
                           "(all its ids) was removed");
    }

    if (findings.empty()) return result;  // additive / fresh only -> boot + record

    // Assemble the actionable refusal report (mirrors `mcc diff`).
    result.breaking = true;
    result.to_record.clear();  // record NOTHING on a refusal
    std::string report =
        "BREAKING pack change requires an operator migration before this realm "
        "can boot (SP2 boot compat gate, spec §2.5 / umbrella §6.5).\n"
        "  classification: BREAKING";
    for (const std::string& f : findings) report += "\n  " + f;
    report +=
        "\n  The realm's persisted characters may reference content that changed "
        "incompatibly, so worldd will not serve this pack against the recorded "
        "realm state."
        "\n  To resolve: run `mcc diff <old-pack> <new-pack>` to see exactly which "
        "ids/fields broke, migrate the realm's character data accordingly, then "
        "advance realm_content_state.compatibility_version to match the loaded pack.";
    result.reason = std::move(report);
    return result;
}

std::vector<RealmCompatRow> read_realm_compat_state(db::Connection& char_db) {
    db::Result res = char_db.execute(
        "SELECT pack_namespace, compatibility_version, content_hash, pack_version "
        "FROM realm_content_state ORDER BY pack_namespace");

    std::vector<RealmCompatRow> rows;
    rows.reserve(res.rows.size());
    for (const db::Row& r : res.rows) {
        RealmCompatRow m;
        m.pack_namespace        = cell_str(r[0]);
        m.compatibility_version = cell_u32(r[1]);
        m.content_hash          = cell_str(r[2]);
        m.pack_version          = cell_str(r[3]);
        rows.push_back(std::move(m));
    }
    return rows;
}

void record_realm_compat_state(db::Connection& char_db,
                               const std::vector<ManifestRow>& rows) {
    // UPSERT one row per pack. Parameterized (never concatenate values into SQL).
    // ON DUPLICATE KEY UPDATE keeps the recorded_at fresh (DDL ON UPDATE) and
    // refreshes the context columns; the compatibility_version only ever changes
    // here to the loaded value the gate already deemed SAFE (fresh/compatible).
    for (const ManifestRow& r : rows) {
        char_db.execute(
            "INSERT INTO realm_content_state "
            "(pack_namespace, compatibility_version, content_hash, pack_version) "
            "VALUES (?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE "
            "compatibility_version = VALUES(compatibility_version), "
            "content_hash = VALUES(content_hash), "
            "pack_version = VALUES(pack_version)",
            {db::Param{r.pack_namespace},
             db::Param{static_cast<std::int64_t>(r.compatibility_version)},
             db::Param{r.content_hash},
             db::Param{r.pack_version}});
    }
}

// ---------------------------------------------------------------------------
// Full boot check + logging
// ---------------------------------------------------------------------------

BootReport boot_world_db(db::Connection& world_db,
                         const std::optional<std::string>& expected_content_hash,
                         bool require_content,
                         db::Connection* char_db) {
    BootReport rep;
    std::vector<ManifestRow> rows;
    try {
        rows = read_world_manifest(world_db);
        rep = verify_world_manifest(rows, expected_content_hash);
    } catch (const db::DbError& e) {
        // A read failure — most commonly the world_manifest table does not exist
        // (an un-loaded / not-yet-seeded world DB). Treat as a missing manifest;
        // never let the boot check throw past this point. This joins the empty
        // manifest in the degrade policy below (both are the "no content yet"
        // shape #485 targets — the seed either has not created the table or has
        // not written any rows).
        rep.verdict = BootVerdict::kMissingManifest;
        rep.hard_fail = true;
        rep.reason = std::string("could not read world_manifest: ") + e.what();
    }

    // Degrade policy (#485): a configured-but-empty / missing world DB
    // (kMissingManifest) is NOT fatal by default — worldd serves WITHOUT content
    // and self-heals once the content-seed lands, rather than exiting into a k8s
    // CrashLoopBackOff when it restarts before the seed Job finishes. Opt into the
    // strict fail-fast with MERIDIAN_WORLDDB_REQUIRE_CONTENT=1 (require_content).
    // INTEGRITY faults (kMalformedManifest / kSchemaMismatch — corrupt or
    // unserveable content) are unaffected: they always hard-fail.
    if (rep.verdict == BootVerdict::kMissingManifest && !require_content) {
        rep.hard_fail = false;
        rep.degraded = true;
    }

    // Boot-time compat / migration gate (#698). Runs ONLY when a characters DB is
    // wired AND the manifest is present + serveable (kOk / kSoftWarn) — never on a
    // degraded / missing / malformed / schema-mismatched manifest (nothing to
    // gate). Compares the loaded packs against the realm's persisted state; a
    // breaking mismatch turns this into a hard fail (kCompatBreaking), otherwise it
    // RECORDS the loaded state ("worldd records the pack compatibility_version it
    // booted with"). A characters-DB error propagates (the caller fails the boot).
    if (char_db != nullptr && !rep.hard_fail && !rep.degraded &&
        (rep.verdict == BootVerdict::kOk || rep.verdict == BootVerdict::kSoftWarn)) {
        const std::vector<RealmCompatRow> persisted = read_realm_compat_state(*char_db);
        const CompatGateResult gate = verify_compat_gate(rows, persisted);
        if (gate.breaking) {
            rep.verdict = BootVerdict::kCompatBreaking;
            rep.hard_fail = true;
            rep.reason = gate.reason;
        } else {
            record_realm_compat_state(*char_db, gate.to_record);
        }
    }

    if (rep.degraded) {
        // Loud ERROR so the unseeded state is unmissable in logs — but worldd
        // boots content-less and recovers automatically once the seed lands.
        core::log::error(
            kCat, "world-DB boot DEGRADED [" +
                      std::string(boot_verdict_name(rep.verdict)) + "] — " +
                      rep.reason +
                      "; serving WITHOUT content (realm self-heals once content is "
                      "seeded). Set MERIDIAN_WORLDDB_REQUIRE_CONTENT=1 to fail fast "
                      "instead.");
    } else if (rep.verdict == BootVerdict::kOk) {
        // The prominent success line: which content version + hash worldd serves
        // (SAD §4.3 "logs the content hash"; runbook §Content-visibility / TLS-01
        // "content-hash logged by worldd on boot").
        core::log::info(
            kCat, "world-DB boot OK — content " + rep.content_version +
                      " schema v" + std::to_string(rep.schema_version) + " hash " +
                      rep.content_hash + " (" + std::to_string(rep.pack_count) +
                      " pack" + (rep.pack_count == 1 ? "" : "s") + ")");
    } else if (rep.verdict == BootVerdict::kSoftWarn) {
        // Bootable but loud — the advisory content-hash tie.
        core::log::warn(kCat, "world-DB boot WARNING — " + rep.reason);
        core::log::info(kCat, "world-DB boot proceeding — content " +
                                  rep.content_version + " hash " + rep.content_hash);
    } else {
        // Hard fail — the caller refuses to boot.
        core::log::error(kCat, "world-DB boot FAILED [" +
                                   std::string(boot_verdict_name(rep.verdict)) +
                                   "] — " + rep.reason);
    }
    return rep;
}

}  // namespace meridian::worldd
