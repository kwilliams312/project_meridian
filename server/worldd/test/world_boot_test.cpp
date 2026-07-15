// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB boot: manifest verify UNIT TEST (issue #89, IF-4 content
// contract).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §4.3 (the boot handshake:
// worldd reads the manifest, refuses to start on a schema-version mismatch, logs
// the content hash), §5.4.3 (the content-hash tie: warning M0–M1), and the world
// DDL schema/sql/world/00_manifest.sql (the world_manifest columns). No GPL
// source consulted (CONTRIBUTING).
//
// PURE / DB-FREE: exercises verify_world_manifest directly over in-memory rows —
// no DB, no socket, no I/O. It therefore runs in the PLAIN server ctest (build.yml
// `server` job), no MariaDB service needed. This is the "the pure hash/verify
// logic should ALSO be unit-testable DB-free" ask (#89). The DB-backed boot proof
// (a real Connection over a loaded world DDL) is world_boot_db_test.cpp.
//
// What it proves:
//   A. A GOOD manifest (well-formed, matching schema) -> kOk, hard_fail=false,
//      and the resolved content version/hash come from the "core" primary pack.
//   B. A MISSING manifest (zero rows) -> kMissingManifest, hard_fail=true.
//   C. A MALFORMED content hash (wrong width / non-hex / uppercase) ->
//      kMalformedManifest, hard_fail=true (a truncated / corrupt load).
//   D. An empty required field (pack_namespace) -> kMalformedManifest.
//   E. A SCHEMA-VERSION mismatch -> kSchemaMismatch, hard_fail=true (a world DB
//      this binary cannot serve).
//   F. A pinned EXPECTED-HASH mismatch -> kSoftWarn, hard_fail=FALSE (advisory
//      content-hash tie, still bootable at M0–M1).
//   G. A pinned expected hash that MATCHES -> kOk (no warning).
//   H. A MALFORMED pin is ignored (treated as no-pin) -> kOk, not a fail.
//   I. Multi-pack: "core" is chosen as the primary over an alphabetically-earlier
//      namespace; one bad pack among good ones fails the whole boot.

#include "world_boot.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// A valid 64-lowercase-hex content hash (BLAKE3 rendering shape).
const std::string kGoodHash =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kOtherHash =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

ManifestRow good_row(const std::string& ns = "core",
                     const std::string& ver = "1.0.0",
                     const std::string& hash = kGoodHash,
                     std::uint32_t schema = kSupportedContentSchemaVersion) {
    ManifestRow r;
    r.pack_namespace = ns;
    r.pack_version = ver;
    r.id_band = 1000;
    r.content_hash = hash;
    r.schema_version = schema;
    r.mcc_version = "mcc-0.1.0";
    r.built_at = "2026-07-06 00:00:00";
    return r;
}

// A loaded manifest row carrying a specific compatibility_version (for the gate).
ManifestRow loaded_pack(const std::string& ns, std::uint32_t compat,
                        const std::string& hash = kGoodHash) {
    ManifestRow r = good_row(ns, "1.0.0", hash);
    r.compatibility_version = compat;
    return r;
}

// A persisted realm-state row (what the realm last booted with) for the gate.
RealmCompatRow persisted_pack(const std::string& ns, std::uint32_t compat) {
    RealmCompatRow r;
    r.pack_namespace = ns;
    r.compatibility_version = compat;
    r.content_hash = kGoodHash;
    r.pack_version = "1.0.0";
    return r;
}

}  // namespace

int main() {
    std::printf("worldd world-DB boot verify (IF-4 manifest) unit test\n");

    // A. Good single-pack manifest -> OK, identity resolved from the pack.
    {
        BootReport r = verify_world_manifest({good_row()});
        check("A good manifest -> kOk", r.verdict == BootVerdict::kOk);
        check("A good manifest -> not hard_fail", !r.hard_fail);
        check("A resolves content_hash", r.content_hash == kGoodHash);
        check("A resolves content_version core@1.0.0",
              r.content_version == "core@1.0.0");
        check("A resolves schema_version 1", r.schema_version == 1);
        check("A pack_count 1", r.pack_count == 1);
    }

    // B. Missing / empty manifest -> hard fail.
    {
        BootReport r = verify_world_manifest({});
        check("B empty manifest -> kMissingManifest",
              r.verdict == BootVerdict::kMissingManifest);
        check("B empty manifest -> hard_fail", r.hard_fail);
    }

    // C. Malformed content hash (each variant is a truncated / corrupt load).
    {
        BootReport too_short = verify_world_manifest({good_row("core", "1.0.0", "abc123")});
        check("C short hash -> kMalformedManifest",
              too_short.verdict == BootVerdict::kMalformedManifest && too_short.hard_fail);

        std::string non_hex = kGoodHash;
        non_hex[10] = 'g';  // 'g' is not a hex digit
        BootReport bad_char = verify_world_manifest({good_row("core", "1.0.0", non_hex)});
        check("C non-hex char -> kMalformedManifest",
              bad_char.verdict == BootVerdict::kMalformedManifest && bad_char.hard_fail);

        std::string upper = kGoodHash;
        upper[0] = 'A';  // uppercase — mcc renders lowercase, so this is malformed
        BootReport upper_rep = verify_world_manifest({good_row("core", "1.0.0", upper)});
        check("C uppercase hex -> kMalformedManifest",
              upper_rep.verdict == BootVerdict::kMalformedManifest && upper_rep.hard_fail);
    }

    // D. Empty required field.
    {
        BootReport r = verify_world_manifest({good_row("")});
        check("D empty namespace -> kMalformedManifest",
              r.verdict == BootVerdict::kMalformedManifest && r.hard_fail);
    }

    // E. Schema-version mismatch -> hard fail (SAD §4.3).
    {
        BootReport r = verify_world_manifest(
            {good_row("core", "1.0.0", kGoodHash, kSupportedContentSchemaVersion + 1)});
        check("E schema mismatch -> kSchemaMismatch",
              r.verdict == BootVerdict::kSchemaMismatch);
        check("E schema mismatch -> hard_fail", r.hard_fail);
    }

    // F. Pinned expected-hash mismatch -> SoftWarn, still bootable (SAD §5.4.3).
    {
        BootReport r = verify_world_manifest({good_row()}, kOtherHash);
        check("F pinned-hash mismatch -> kSoftWarn", r.verdict == BootVerdict::kSoftWarn);
        check("F pinned-hash mismatch -> NOT hard_fail (advisory M0–M1)", !r.hard_fail);
        check("F still resolves the LOADED hash", r.content_hash == kGoodHash);
    }

    // G. Pinned expected hash that matches -> OK, no warning.
    {
        BootReport r = verify_world_manifest({good_row()}, kGoodHash);
        check("G matching pin -> kOk", r.verdict == BootVerdict::kOk && !r.hard_fail);
    }

    // H. Malformed pin -> ignored (no-pin), not a fail.
    {
        BootReport r = verify_world_manifest({good_row()}, std::string("not-a-hash"));
        check("H malformed pin ignored -> kOk", r.verdict == BootVerdict::kOk && !r.hard_fail);
    }

    // I. Multi-pack: "core" is the primary even when another namespace sorts
    //    earlier; a single bad pack fails the whole boot.
    {
        std::vector<ManifestRow> rows = {
            good_row("aardvark", "2.0.0", kOtherHash),  // sorts before "core"
            good_row("core", "1.0.0", kGoodHash),
        };
        BootReport r = verify_world_manifest(rows);
        check("I core chosen as primary over 'aardvark'",
              r.verdict == BootVerdict::kOk && r.content_version == "core@1.0.0" &&
                  r.content_hash == kGoodHash);
        check("I pack_count 2", r.pack_count == 2);

        std::vector<ManifestRow> mixed = {
            good_row("core", "1.0.0", kGoodHash),
            good_row("dlc1", "1.0.0", kGoodHash, kSupportedContentSchemaVersion + 5),
        };
        BootReport rm = verify_world_manifest(mixed);
        check("I one bad pack fails the whole boot",
              rm.verdict == BootVerdict::kSchemaMismatch && rm.hard_fail);
    }

    // -----------------------------------------------------------------------
    // Boot-time compat / migration gate (#698) — PURE verify_compat_gate.
    // -----------------------------------------------------------------------

    // J. FRESH realm (no persisted state): every loaded pack is recorded, boots.
    {
        CompatGateResult g = verify_compat_gate({loaded_pack("core", 1)}, {});
        check("J fresh realm -> not breaking", !g.breaking);
        check("J fresh realm -> records the loaded pack", g.to_record.size() == 1);
        check("J fresh realm -> no refusal reason", g.reason.empty());
    }

    // K. COMPATIBLE: loaded compatibility_version equals the persisted one
    //    (identical, or an additive change that never bumps it) -> boots + records.
    {
        CompatGateResult g =
            verify_compat_gate({loaded_pack("core", 3)}, {persisted_pack("core", 3)});
        check("K compatible -> not breaking", !g.breaking);
        check("K compatible -> re-records the pack", g.to_record.size() == 1);
    }

    // L. BREAKING forward: loaded compatibility_version > persisted -> refuse,
    //    record nothing, name the offending pack.
    {
        CompatGateResult g =
            verify_compat_gate({loaded_pack("core", 2)}, {persisted_pack("core", 1)});
        check("L breaking forward -> breaking", g.breaking);
        check("L breaking forward -> records nothing", g.to_record.empty());
        check("L breaking forward -> reason names the pack + versions",
              g.reason.find("core") != std::string::npos &&
                  g.reason.find("1 (realm last booted) -> 2 (loaded pack)") !=
                      std::string::npos);
        check("L breaking forward -> reason is actionable (mcc diff + migrate)",
              g.reason.find("mcc diff") != std::string::npos &&
                  g.reason.find("BREAKING") != std::string::npos);
    }

    // M. BREAKING rollback: loaded compatibility_version < persisted -> refuse.
    {
        CompatGateResult g =
            verify_compat_gate({loaded_pack("core", 1)}, {persisted_pack("core", 4)});
        check("M breaking rollback -> breaking", g.breaking);
        check("M breaking rollback -> reason notes the rollback",
              g.reason.find("rolled back") != std::string::npos);
    }

    // N. REMOVED pack: a persisted pack absent from the loaded set -> refuse.
    {
        CompatGateResult g = verify_compat_gate(
            {loaded_pack("core", 1)},
            {persisted_pack("core", 1), persisted_pack("dlc1", 1)});
        check("N removed pack -> breaking", g.breaking);
        check("N removed pack -> names the vanished pack",
              g.reason.find("dlc1") != std::string::npos &&
                  g.reason.find("ABSENT") != std::string::npos);
    }

    // O. MULTI-PACK: one compatible + one breaking -> refuse overall, and the
    //    compatible pack is NOT recorded (record nothing on any refusal).
    {
        CompatGateResult g = verify_compat_gate(
            {loaded_pack("core", 1), loaded_pack("dlc1", 2)},
            {persisted_pack("core", 1), persisted_pack("dlc1", 1)});
        check("O mixed -> breaking", g.breaking);
        check("O mixed -> records nothing (even the compatible pack)",
              g.to_record.empty());
        check("O mixed -> reason names only the breaking pack",
              g.reason.find("dlc1") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // P. Realm theme-selection seam (#754, chibi-theme design §4). The primary
    //    pack is the row whose pack_namespace == the realm's selected theme; the
    //    default theme is "core" (historical behaviour); an absent theme falls
    //    back to the first row.
    // -----------------------------------------------------------------------
    {
        std::vector<ManifestRow> rows = {
            good_row("chibi", "2.0.0", kOtherHash),  // sorts before "core"
            good_row("core", "1.0.0", kGoodHash),
        };

        // P1. Default (no theme arg) resolves "core" as primary — unchanged.
        BootReport def = verify_world_manifest(rows);
        check("P1 default theme -> core primary",
              def.verdict == BootVerdict::kOk &&
                  def.content_version == "core@1.0.0" &&
                  def.content_hash == kGoodHash);

        // P2. theme="chibi" resolves the chibi row as primary (the §4 seam).
        BootReport chibi = verify_world_manifest(rows, std::nullopt, "chibi");
        check("P2 theme=chibi -> chibi primary",
              chibi.verdict == BootVerdict::kOk &&
                  chibi.content_version == "chibi@2.0.0" &&
                  chibi.content_hash == kOtherHash);

        // P3. theme="core" explicit -> identical to the default (core primary).
        BootReport core = verify_world_manifest(rows, std::nullopt, "core");
        check("P3 theme=core -> core primary (unchanged)",
              core.verdict == BootVerdict::kOk &&
                  core.content_version == "core@1.0.0");

        // P4. A selected theme absent from the manifest -> first-row fallback
        //     (the existing 'first row' behaviour, generalised).
        BootReport missing = verify_world_manifest(rows, std::nullopt, "nonesuch");
        check("P4 absent theme -> first-row fallback",
              missing.verdict == BootVerdict::kOk &&
                  missing.content_version == "chibi@2.0.0");  // rows.front()
    }

    // verdict name mapping (diagnostics).
    check("boot_verdict_name kOk", std::string(boot_verdict_name(BootVerdict::kOk)) == "ok");
    check("boot_verdict_name kCompatBreaking",
          std::string(boot_verdict_name(BootVerdict::kCompatBreaking)) == "compat-breaking");

    if (g_fail == 0) {
        std::printf("PASS: all world-DB boot verify checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d world-DB boot verify check(s) failed\n", g_fail);
    return 1;
}
