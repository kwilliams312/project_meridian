// SPDX-License-Identifier: Apache-2.0
//
// worldd — ENTER-WORLD START-ZONE SPAWN load DB-backed integration test (C8
// enter-as-chibi, story #761, epic #753). The acceptance bar for C8: a character
// entering the world must spawn at its realm's START ZONE first graveyard (per
// zone.schema.yaml — "start_zone: spawn point = first graveyard"), NOT the old
// D-11 PLACEHOLDER (movement::kZoneSpawnXY, the Zone-01 flat-ground centre). This
// test proves the server-side resolution the enter-world handler now depends on:
//
//   load_start_zone_spawn(world_db) resolves the (single) zone whose start_zone is
//   TRUE, joins it to its ordinal-0 graveyard, converts the position from the DB's
//   Godot Y-up frame to the worldd Z-up runtime frame (the #498 conversion), and
//   returns it — so main() installs it (WorldServer::set_enter_spawn) and the
//   ENTER_WORLD handler spawns the character there instead of the placeholder.
//
// It asserts the full contract:
//   * SELECTION  — a NON-start zone's graveyard is ignored; only start_zone wins.
//   * ORDINAL    — the FIRST graveyard (ordinal 0) is the spawn, not a later one.
//   * AXIS       — the position is converted Godot Y-up (pos_x east, pos_y height,
//                  pos_z north) -> server Z-up (x east planar, y north planar, z
//                  height), matching load_spawn_points; the graveyard facing
//                  (orientation_deg) becomes the Position orientation in radians.
//   * CHIBI      — the real chibi shape (Sprout Meadow graveyard at ORIGIN) resolves
//                  to a (0,0,0) spawn — the dev chibi realm's enter point.
//   * DEGRADE    — no start zone (or a start zone with no graveyard) -> std::nullopt,
//                  so the handler keeps the D-11 placeholder (DB-less / degraded).
//
// Self-seeded, DB-LEVEL ISOLATED (#707): the loader hard-codes the `zone` /
// `graveyard` table names, so isolation is at the DATABASE level — the test creates
// a UNIQUE throwaway database keyed by pid (parallel-safe), uses it, seeds only the
// two tables load_start_zone_spawn reads, and drops the whole database at the end.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0)
// when neither is set, so it is inert in the plain server ctest and runs for real
// only with a live MariaDB (test.sh --db / the DB CI job).
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL
// (schema/sql/world/80_zone.sql) + the #761 seam; no GPL/AGPL/CMaNGOS/TrinityCore/
// leaked source consulted).

#include "db_content_store.h"

#include "meridian/db/connection.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

using namespace meridian;
namespace db = meridian::db;
namespace mw = meridian::worldd;

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

bool approx(float a, float b, float eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

// The two tables load_start_zone_spawn reads, created WITHOUT foreign keys (the
// loader only SELECTs). Columns mirror schema/sql/world/80_zone.sql.
void create_tables(db::Connection& c) {
    c.execute("DROP TABLE IF EXISTS graveyard");
    c.execute("DROP TABLE IF EXISTS zone");
    c.execute(
        "CREATE TABLE zone ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  level_min SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  level_max SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  start_zone BOOLEAN NOT NULL DEFAULT FALSE,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE graveyard ("
        "  zone_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
        "  orientation_deg FLOAT NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (zone_id, ordinal))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void truncate_all(db::Connection& c) {
    c.execute("DELETE FROM graveyard");
    c.execute("DELETE FROM zone");
}

}  // namespace

int main() {
    std::printf("worldd enter-world start-zone spawn load DB-backed test (#761)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* sk = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        p.unix_socket = sk; configured = true;
    }
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST")) {
        p.host = h; configured = true;
    }
    if (const char* port = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT"))
        p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS")) p.password = pw;
    if (const char* n = pick("MERIDIAN_WORLDDB_NAME", "MERIDIAN_DB_NAME")) p.database = n;

    if (!configured) {
        std::printf("SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured — "
                    "enter-spawn checks skipped\n");
        return 0;
    }

    // DB-LEVEL ISOLATION (#707): a unique throwaway database keyed by pid so parallel
    // test processes never collide on the shared `zone`/`graveyard` table names.
    const std::string dbname =
        "meridian_enter_spawn_test_" + std::to_string(static_cast<long>(getpid()));

    try {
        db::Connection conn(p);
        conn.execute("DROP DATABASE IF EXISTS " + dbname);
        conn.execute("CREATE DATABASE " + dbname +
                     " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
        conn.execute("USE " + dbname);
        create_tables(conn);

        // ---- Scenario A: SELECTION + ORDINAL + AXIS in one shot -----------------
        // A start zone (Sprout-Meadow-shaped id in the chibi band) whose ordinal-0
        // graveyard is at a DISTINCT non-origin Godot Y-up position, plus two decoys
        // the loader must ignore: an ordinal-1 graveyard in the same zone, and an
        // ordinal-0 graveyard in a NON-start zone.
        constexpr std::uint32_t kStartZone = 1048578;  // chibi band (1<<20 + 2)
        constexpr std::uint32_t kOtherZone = 5;
        conn.execute(
            "INSERT INTO zone (id, name, level_min, level_max, start_zone) "
            "VALUES (1048578, 'Sprout Meadow', 1, 5, TRUE)");
        conn.execute(
            "INSERT INTO zone (id, name, level_min, level_max, start_zone) "
            "VALUES (5, 'Gloom Dungeon', 10, 20, FALSE)");
        // Start-zone ordinal-0 graveyard: Godot Y-up (pos_x=10 east, pos_y=3.5 HEIGHT,
        // pos_z=-20 north), facing 90 deg. The #498 conversion must yield server
        // (x=10 planar east, y=-20 planar north from pos_z, z=3.5 height from pos_y).
        conn.execute(
            "INSERT INTO graveyard (zone_id, ordinal, pos_x, pos_y, pos_z, orientation_deg) "
            "VALUES (1048578, 0, 10, 3.5, -20, 90)");
        // DECOY 1 — a later graveyard (ordinal 1) in the start zone: must NOT be picked.
        conn.execute(
            "INSERT INTO graveyard (zone_id, ordinal, pos_x, pos_y, pos_z, orientation_deg) "
            "VALUES (1048578, 1, 500, 0, 500, 0)");
        // DECOY 2 — the non-start zone's ordinal-0 graveyard: must NOT be picked.
        conn.execute(
            "INSERT INTO graveyard (zone_id, ordinal, pos_x, pos_y, pos_z, orientation_deg) "
            "VALUES (5, 0, 999, 0, 999, 0)");

        std::optional<mw::EnterSpawn> es = mw::load_start_zone_spawn(conn);
        check("A: a start-zone spawn is resolved", es.has_value());
        if (es) {
            check("A: SELECTION — the START ZONE is chosen (not the non-start zone)",
                  es->zone_id == kStartZone);
            check("A: AXIS — planar east x = pos_x (10)", approx(es->pos.x, 10.0f));
            check("A: AXIS — planar north y = pos_z (-20), NOT the height (3.5)",
                  approx(es->pos.y, -20.0f));
            check("A: AXIS — height z = pos_y (3.5), NOT the planar-north (-20)",
                  approx(es->pos.z, 3.5f));
            check("A: ORDINAL — the FIRST graveyard (ord 0) is the spawn, not ord 1 (500,*,500)",
                  !approx(es->pos.x, 500.0f) && !approx(es->pos.y, 500.0f));
            check("A: SELECTION — the non-start decoy (999,*,999) is ignored",
                  !approx(es->pos.x, 999.0f));
            check("A: FACING — orientation_deg (90) -> radians (pi/2)",
                  approx(es->pos.orientation,
                         90.0f * 3.14159265358979323846f / 180.0f, 1.0e-3f));
        }
        (void)kOtherZone;

        // ---- Scenario B: CHIBI shape — Sprout Meadow graveyard at ORIGIN --------
        // The real chibi pack's Sprout Meadow ships one graveyard at (0,0,0). Reseed
        // that exact shape and prove the enter spawn is the origin — the dev chibi
        // realm's enter point.
        truncate_all(conn);
        conn.execute(
            "INSERT INTO zone (id, name, level_min, level_max, start_zone) "
            "VALUES (1048578, 'Sprout Meadow', 1, 5, TRUE)");
        conn.execute(
            "INSERT INTO graveyard (zone_id, ordinal, pos_x, pos_y, pos_z, orientation_deg) "
            "VALUES (1048578, 0, 0, 0, 0, 0)");
        es = mw::load_start_zone_spawn(conn);
        check("B: the chibi start-zone spawn resolves", es.has_value());
        if (es) {
            check("B: chibi Sprout Meadow spawn is the origin (0,0,0)",
                  approx(es->pos.x, 0.0f) && approx(es->pos.y, 0.0f) &&
                      approx(es->pos.z, 0.0f));
            check("B: chibi spawn resolves the Sprout Meadow zone id", es->zone_id == kStartZone);
        }

        // ---- Scenario C: DEGRADE — no start zone (or none with a graveyard) -----
        // (c1) A zone that is NOT a start zone, with a graveyard: the loader must
        //      resolve nothing (only start_zone qualifies) -> nullopt.
        truncate_all(conn);
        conn.execute(
            "INSERT INTO zone (id, name, level_min, level_max, start_zone) "
            "VALUES (5, 'Gloom Dungeon', 10, 20, FALSE)");
        conn.execute(
            "INSERT INTO graveyard (zone_id, ordinal, pos_x, pos_y, pos_z, orientation_deg) "
            "VALUES (5, 0, 1, 2, 3, 0)");
        check("C1: no start zone -> nullopt (handler keeps the D-11 placeholder)",
              !mw::load_start_zone_spawn(conn).has_value());

        // (c2) A start zone that ships NO graveyard: the JOIN yields nothing -> nullopt.
        truncate_all(conn);
        conn.execute(
            "INSERT INTO zone (id, name, level_min, level_max, start_zone) "
            "VALUES (1048578, 'Sprout Meadow', 1, 5, TRUE)");
        check("C2: start zone with no graveyard -> nullopt",
              !mw::load_start_zone_spawn(conn).has_value());

        // ---- cleanup ----------------------------------------------------------
        conn.execute("DROP DATABASE IF EXISTS " + dbname);
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        // Best-effort cleanup of the throwaway database on failure.
        try {
            db::Connection cleanup(p);
            cleanup.execute("DROP DATABASE IF EXISTS " + dbname);
        } catch (const db::DbError&) {
        }
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all enter-world start-zone spawn checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d enter-spawn check(s) failed\n", g_fail);
    return 1;
}
