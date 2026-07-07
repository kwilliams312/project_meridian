// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB boot: DB-backed boot INTEGRATION TEST (issue #89, IF-4).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §4.3 (the boot handshake over
// the read-only world DB), §5.4.3 (content-hash tie), and the world DDL
// schema/sql/world/00_manifest.sql (the world_manifest table this test seeds and
// read_world_manifest reads). No GPL source consulted (CONTRIBUTING).
//
// DB-GATED: reads MERIDIAN_WORLDDB_* env (its own vars, since the world DB is a
// separate DB from the auth DB — SAD §2.2 3-DB split) and SKIPS (exit 0) when
// none are set. Falls back to MERIDIAN_DB_* if the WORLDDB_* vars are unset, so
// the existing worldd-session CI job (which sets MERIDIAN_DB_*) can run this test
// against its mariadb service without new env plumbing. Inert in the plain server
// ctest; runs for real only with a live MariaDB (the worldd-session CI job).
//
// It drives the REAL meridian::db::Connection over a REAL world_manifest table:
//   1. Create the world_manifest table (matching schema/sql/world/00_manifest.sql)
//      + seed a valid "core" pack row.
//   2. read_world_manifest returns exactly that row (the IF-4 read shape).
//   3. boot_world_db -> kOk (bootable), resolves the version + hash.
//   4. Truncate the manifest (delete the row) -> boot_world_db kMissingManifest,
//      hard_fail (the truncated-load fail-fast case).
//   5. Re-seed with a bumped schema_version -> boot_world_db kSchemaMismatch,
//      hard_fail (a world DB this binary cannot serve).
//   6. Re-seed valid but pin a DIFFERENT expected hash -> kSoftWarn, bootable
//      (the advisory content-hash tie).
//
// The test creates + drops the table it owns, so it is idempotent against a DB
// that also has the auth schema loaded (it touches only world_manifest).

#include "meridian/db/connection.h"
#include "world_boot.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian::worldd;
namespace db = meridian::db;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

const std::string kGoodHash =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kOtherHash =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

// DDL identical in shape to schema/sql/world/00_manifest.sql (the columns
// read_world_manifest SELECTs). Created here so the test is self-contained
// against any MariaDB; the CI job additionally loads the real *.sql files.
void ensure_manifest_table(db::Connection& db) {
    db.execute("DROP TABLE IF EXISTS world_manifest");
    db.execute(
        "CREATE TABLE world_manifest ("
        "  pack_namespace  VARCHAR(32)  NOT NULL,"
        "  pack_version    VARCHAR(32)  NOT NULL,"
        "  id_band         INT UNSIGNED NOT NULL,"
        "  content_hash    CHAR(64)     NOT NULL,"
        "  schema_version  INT UNSIGNED NOT NULL,"
        "  mcc_version     VARCHAR(32)  NOT NULL,"
        "  built_at        DATETIME     NOT NULL,"
        "  PRIMARY KEY (pack_namespace)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void seed_core_pack(db::Connection& db, const std::string& hash,
                    std::uint32_t schema_version) {
    db.execute("DELETE FROM world_manifest");
    db.execute(
        "INSERT INTO world_manifest "
        "(pack_namespace, pack_version, id_band, content_hash, schema_version, "
        " mcc_version, built_at) "
        "VALUES ('core', '1.0.0', 1000, ?, ?, 'mcc-0.1.0', '2026-07-06 00:00:00')",
        {db::Param{hash},
         db::Param{static_cast<std::int64_t>(schema_version)}});
}

}  // namespace

int main() {
    std::printf("worldd world-DB boot DB-backed integration test (IF-4)\n");

    // Prefer MERIDIAN_WORLDDB_*; fall back to MERIDIAN_DB_* so the existing
    // worldd-session CI job (which sets MERIDIAN_DB_*) can run this unchanged.
    db::ConnectParams p;
    bool configured = false;
    auto pick = [&](const char* world_key, const char* fallback_key) -> const char* {
        if (const char* v = env(world_key)) return v;
        return env(fallback_key);
    };
    if (const char* s = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        p.unix_socket = s; configured = true;
    }
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST")) {
        p.host = h; configured = true;
    }
    if (const char* port = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT")) {
        p.port = static_cast<unsigned>(std::atoi(port));
    }
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS")) p.password = pw;
    if (const char* n = pick("MERIDIAN_WORLDDB_NAME", "MERIDIAN_DB_NAME")) p.database = n;

    if (!configured) {
        std::printf("SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured "
                    "— DB-backed boot checks skipped (set MERIDIAN_WORLDDB_HOST + "
                    "MERIDIAN_WORLDDB_USER, or reuse MERIDIAN_DB_*)\n");
        return 0;
    }

    try {
        db::Connection conn(p);
        ensure_manifest_table(conn);

        // 1 + 2. Seed a valid core pack; read it back.
        seed_core_pack(conn, kGoodHash, kSupportedContentSchemaVersion);
        std::vector<ManifestRow> rows = read_world_manifest(conn);
        check("read_world_manifest returns 1 row", rows.size() == 1);
        if (rows.size() == 1) {
            check("row namespace core", rows[0].pack_namespace == "core");
            check("row content_hash roundtrips", rows[0].content_hash == kGoodHash);
            check("row schema_version 1",
                  rows[0].schema_version == kSupportedContentSchemaVersion);
            check("row id_band 1000", rows[0].id_band == 1000);
        }

        // 3. boot_world_db over a real connection -> kOk.
        {
            BootReport r = boot_world_db(conn);
            check("valid manifest -> kOk", r.verdict == BootVerdict::kOk);
            check("valid manifest -> bootable", !r.hard_fail);
            check("valid manifest resolves core@1.0.0",
                  r.content_version == "core@1.0.0");
            check("valid manifest resolves the hash", r.content_hash == kGoodHash);
        }

        // 4. Truncated manifest (row deleted) -> fail-fast.
        {
            conn.execute("DELETE FROM world_manifest");
            BootReport r = boot_world_db(conn);
            check("empty manifest -> kMissingManifest",
                  r.verdict == BootVerdict::kMissingManifest);
            check("empty manifest -> hard_fail (fail-fast)", r.hard_fail);
        }

        // 5. Schema-version bump -> fail-fast (unserveable content).
        {
            seed_core_pack(conn, kGoodHash, kSupportedContentSchemaVersion + 1);
            BootReport r = boot_world_db(conn);
            check("schema bump -> kSchemaMismatch",
                  r.verdict == BootVerdict::kSchemaMismatch);
            check("schema bump -> hard_fail (fail-fast)", r.hard_fail);
        }

        // 6. Valid manifest, pinned mismatching expected hash -> SoftWarn (boots).
        {
            seed_core_pack(conn, kGoodHash, kSupportedContentSchemaVersion);
            BootReport r = boot_world_db(conn, kOtherHash);
            check("pinned-hash mismatch -> kSoftWarn", r.verdict == BootVerdict::kSoftWarn);
            check("pinned-hash mismatch -> bootable (advisory)", !r.hard_fail);
        }

        // Cleanup — drop the table this test owns.
        conn.execute("DROP TABLE IF EXISTS world_manifest");
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all DB-backed world-DB boot checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d DB-backed world-DB boot check(s) failed\n", g_fail);
    return 1;
}
