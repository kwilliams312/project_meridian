// tools/mcc/tests/test_emit_sql_db.cpp — DB-backed emit-sql INTEGRATION test
// (Tools SAD §2.6, IF-4). Loads the REAL world DDL (schema/sql/world/*.sql) plus
// the mcc-emitted content SQL into a live MariaDB and asserts the content tables
// AND world_manifest are populated correctly.
//
// ENV-GUARDED (parity with the worldd DB tests, server/worldd/test/
// world_boot_db_test.cpp): reads MERIDIAN_WORLDDB_* (falling back to MERIDIAN_DB_*)
// and SKIPS (exit 0) when no connection is configured. It shells out to the
// `mariadb` (or `mysql`) client rather than linking the MariaDB client lib, so
// mcc's dependency surface stays at just yaml-cpp — the SQL is what a real
// `mysql < world.sql` load would run.
//
// What it proves:
//   1. The world DDL + the mcc-emitted SQL load together WITHOUT error (FK
//      ordering handled by the emitted FOREIGN_KEY_CHECKS bracket).
//   2. world_manifest has exactly one 'core' row with schema_version=1 and a
//      64-hex content_hash — the shape worldd's boot (#89) accepts.
//   3. Content tables (npc_template, item_template, ability, zone, ...) are
//      populated, keyed by IF-9 numeric ids.
//   4. A ref column resolved to a numeric id points at a real row (referential
//      integrity holds after the load with FK checks restored).
//
// The test creates + drops a throwaway database it owns, so it is idempotent and
// touches nothing else on the server.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "stages/check.h"

namespace fs = std::filesystem;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }
const char* pick(const char* world_key, const char* fallback_key) {
    if (const char* v = env(world_key)) return v;
    return env(fallback_key);
}

// Locate a working MariaDB/MySQL client binary. Returns "" if none is on PATH.
std::string find_client() {
    for (const char* name : {"mariadb", "mysql"}) {
        std::string cmd = std::string(name) + " --version >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) return name;
    }
    return "";
}

// Build the shared connection flags for the client from the env.
std::string conn_flags() {
    std::string f;
    auto add = [&](const std::string& s) { f += " " + s; };
    if (const char* s = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET"))
        add("--socket=" + std::string(s));
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST"))
        add("--host=" + std::string(h));
    if (const char* p = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT"))
        add("--port=" + std::string(p));
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER"))
        add("--user=" + std::string(u));
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS"))
        add("--password=" + std::string(pw));
    return f;
}

// Run a SQL script file against `db` (empty db = no default database selected).
// Returns the client's exit code.
int run_sql_file(const std::string& client, const std::string& flags,
                 const std::string& db, const fs::path& file) {
    std::string cmd = client + flags;
    if (!db.empty()) cmd += " " + db;
    cmd += " < " + file.string();
    return std::system(cmd.c_str());
}

// Run a one-off query against `db`, capturing the first cell of output (tab/
// newline stripped). Uses --batch --skip-column-names for a bare value.
std::string query_scalar(const std::string& client, const std::string& flags,
                         const std::string& db, const std::string& sql,
                         const fs::path& scratch) {
    const fs::path outp = scratch / "q.out";
    std::string cmd = client + flags + " --batch --skip-column-names " + db + " -e " +
                      "\"" + sql + "\" > " + outp.string() + " 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) return "<error>";
    std::ifstream in(outp);
    std::string line;
    std::getline(in, line);
    // Strip a trailing \r and any whitespace.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

}  // namespace

int main() {
    std::printf("mcc emit-sql DB-backed integration test (IF-4)\n");

    if (!pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST") &&
        !pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        std::printf("SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured "
                    "— DB-backed emit-sql load skipped (set MERIDIAN_WORLDDB_HOST or "
                    "MERIDIAN_DB_HOST, or reuse MERIDIAN_DB_*)\n");
        return 0;
    }

    const std::string client = find_client();
    if (client.empty()) {
        std::printf("SKIP: no mariadb/mysql client on PATH — cannot load the SQL\n");
        return 0;
    }
    const std::string flags = conn_flags();

    // Scratch workspace for the emitted SQL + query output.
    fs::path scratch = fs::temp_directory_path() / "mcc_emit_db_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    // 1. Emit the world DB SQL for the sample content into a file.
    const fs::path world_sql = scratch / "world.sql";
    {
        std::ofstream sink(scratch / "diag.txt");
        const int rc = mcc::stages::emit_sql_content(
            MCC_SAMPLE_CONTENT_DIR, world_sql.string(), "test-db-1.0.0",
            "2026-07-06 12:00:00", mcc::stages::DiagFormat::Text, sink, sink);
        check("emit-sql produced world.sql", rc == 0 && fs::exists(world_sql));
        if (rc != 0) {
            std::printf("FAIL: emit-sql returned %d; aborting DB test\n", rc);
            return 1;
        }
    }

    // 2. Concatenate the world DDL files (00..90) in filename order into one load
    //    script (mcc concatenates the DDL the same way at build time, SAD §2.6).
    const fs::path ddl_sql = scratch / "ddl.sql";
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(MCC_WORLD_DDL_DIR)) {
            if (e.path().extension() == ".sql") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        std::ofstream out(ddl_sql);
        for (const auto& f : files) {
            std::ifstream in(f);
            out << in.rdbuf() << "\n";
        }
        check("assembled world DDL script", fs::exists(ddl_sql) && !files.empty());
    }

    const std::string db = "mcc_emit_it_test";
    // Fresh database.
    {
        const fs::path drop = scratch / "drop.sql";
        std::ofstream(drop) << "DROP DATABASE IF EXISTS " << db << ";\n"
                            << "CREATE DATABASE " << db
                            << " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;\n";
        const int rc = run_sql_file(client, flags, "", drop);
        check("created a fresh test database", rc == 0);
        if (rc != 0) {
            std::printf("FAIL: could not create database (connection/permissions?)\n");
            return 1;
        }
    }

    // 3. Load the DDL, then the emitted DML.
    check("world DDL loads without error", run_sql_file(client, flags, db, ddl_sql) == 0);
    check("mcc-emitted world.sql loads without error",
          run_sql_file(client, flags, db, world_sql) == 0);

    // 4. world_manifest is populated with the worldd-accepted shape.
    check("world_manifest has exactly one row",
          query_scalar(client, flags, db, "SELECT COUNT(*) FROM world_manifest;", scratch) == "1");
    check("world_manifest primary pack is 'core'",
          query_scalar(client, flags, db,
                       "SELECT pack_namespace FROM world_manifest LIMIT 1;", scratch) == "core");
    check("world_manifest schema_version = 1 (worldd's supported major)",
          query_scalar(client, flags, db,
                       "SELECT schema_version FROM world_manifest WHERE pack_namespace='core';",
                       scratch) == "1");
    check("world_manifest compatibility_version emitted (defaults to 1, #698)",
          query_scalar(client, flags, db,
                       "SELECT compatibility_version FROM world_manifest WHERE pack_namespace='core';",
                       scratch) == "1");
    check("world_manifest content_hash is 64 chars (BLAKE3 hex)",
          query_scalar(client, flags, db,
                       "SELECT CHAR_LENGTH(content_hash) FROM world_manifest WHERE pack_namespace='core';",
                       scratch) == "64");
    check("world_manifest content_hash is lowercase hex",
          query_scalar(client, flags, db,
                       "SELECT content_hash=LOWER(content_hash) AND content_hash REGEXP '^[0-9a-f]{64}$' "
                       "FROM world_manifest WHERE pack_namespace='core';", scratch) == "1");

    // 5. Content tables populated, keyed by numeric ids.
    check("npc_template populated",
          std::stoi("0" + query_scalar(client, flags, db,
                    "SELECT COUNT(*) FROM npc_template;", scratch)) >= 3);
    check("item_template populated",
          std::stoi("0" + query_scalar(client, flags, db,
                    "SELECT COUNT(*) FROM item_template;", scratch)) >= 5);
    check("ability populated",
          std::stoi("0" + query_scalar(client, flags, db,
                    "SELECT COUNT(*) FROM ability;", scratch)) >= 2);
    check("zone populated",
          query_scalar(client, flags, db, "SELECT COUNT(*) FROM zone;", scratch) == "1");
    check("spawn_point populated",
          std::stoi("0" + query_scalar(client, flags, db,
                    "SELECT COUNT(*) FROM spawn_point;", scratch)) >= 5);
    // class_race_limit — the SP2.6 #696 race_limits gate. The seed Vanguard(1) is
    // gated to Ardent(1)+Dolmen(2) (2 rows); Warden omits race_limits (no rows).
    check("class_race_limit populated (seed Vanguard gate)",
          std::stoi("0" + query_scalar(client, flags, db,
                    "SELECT COUNT(*) FROM class_race_limit;", scratch)) >= 2);

    // 6. Referential integrity: every quest_template.giver_npc_id points at a real
    //    npc_template row (the *Ref -> numeric id resolution loaded cleanly with
    //    FK checks restored). A dangling join count of 0 means all refs resolved.
    check("quest giver refs resolve to real npc rows",
          query_scalar(client, flags, db,
                       "SELECT COUNT(*) FROM quest_template q "
                       "LEFT JOIN npc_template n ON q.giver_npc_id=n.id "
                       "WHERE n.id IS NULL;", scratch) == "0");
    check("item effect_on_use refs resolve to real ability rows",
          query_scalar(client, flags, db,
                       "SELECT COUNT(*) FROM item_template i "
                       "LEFT JOIN ability a ON i.effect_on_use_id=a.id "
                       "WHERE i.effect_on_use_id IS NOT NULL AND a.id IS NULL;", scratch) == "0");
    // Every class_race_limit row references a real class + a real race (by roster
    // id) — proves emit lowered race_limits[] refs to roster ids that resolve.
    check("class_race_limit class refs resolve to real class rows",
          query_scalar(client, flags, db,
                       "SELECT COUNT(*) FROM class_race_limit crl "
                       "LEFT JOIN class c ON crl.class_roster_id=c.roster_id "
                       "WHERE c.roster_id IS NULL;", scratch) == "0");
    check("class_race_limit race refs resolve to real race rows",
          query_scalar(client, flags, db,
                       "SELECT COUNT(*) FROM class_race_limit crl "
                       "LEFT JOIN race r ON crl.race_roster_id=r.roster_id "
                       "WHERE r.roster_id IS NULL;", scratch) == "0");

    // Cleanup — drop the throwaway database.
    {
        const fs::path drop = scratch / "cleanup.sql";
        std::ofstream(drop) << "DROP DATABASE IF EXISTS " << db << ";\n";
        run_sql_file(client, flags, "", drop);
    }

    if (g_fail == 0) {
        std::printf("PASS: all DB-backed emit-sql checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d DB-backed emit-sql check(s) failed\n", g_fail);
    return 1;
}
