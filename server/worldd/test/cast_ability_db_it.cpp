// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB ABILITY-CATALOG load DB-backed integration test (issue #481).
//
// THE BUG: worldd's live cast path (world_dispatch.cpp CAST_REQUEST handler) built
// its ability catalog from load_placeholder_ability_store(), whose ids are synthetic
// (the 0xF000_0000 band) and do NOT match the authored content ability ids
// (content/core/idmap.lock: minor_healing=1, pickaxe_slam=2). The handler's first
// check is `ctx.abilities->find(ability_id)` — a miss is answered CAST_FAILED
// reason=UNKNOWN_ABILITY. So a client casting an authored id got UNKNOWN_ABILITY for
// EVERY cast. Abilities were the one content type #390's DB content-load never
// covered (it loaded npc/quest/loot/vendor), even though the world DB HAS the
// `ability` / `ability_effect` / `ability_effect_stat_mod` tables and mcc emits them.
//
// THE FIX (#481): load_db_ability_store() reads that authored catalog into an
// AbilityStore keyed by the real ids, installed on the live cast path at boot
// (WorldServer::set_abilities) in place of the placeholder store when a world DB is
// configured.
//
// This test proves the reproduction AND the fix at the store seam the CAST_REQUEST
// handler reads (find()):
//   * REPRODUCTION — the placeholder store does NOT contain authored id 1 or 2, so
//     find() misses -> the handler's UNKNOWN_ABILITY branch (the exact symptom).
//   * FIX — after seeding the authored `ability*` rows and load_db_ability_store(),
//     find(1) resolves to Minor Healing with the AUTHORED metadata (cast/resource/
//     heal), find(2) to Pickaxe Slam (damage + a bleed aura), so the cast STARTS
//     instead of failing UNKNOWN_ABILITY.
//   * PRESERVED — an id absent from the catalog still find()-misses -> UNKNOWN_ABILITY.
//   * effects + aura stat_mods load correctly (a synthetic buff exercises stat_mods).
//
// The seeded rows for ids 1 + 2 are the EXACT values mcc emit-sql produces for
// content/core (tools/mcc/golden/world.sql: minor_healing + pickaxe_slam) — so the
// "metadata matches authored content" assertions are pinned to real content.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0)
// when neither is set — inert in the plain server ctest, real only against a live
// MariaDB (test.sh --db / the DB CI job). Creates + drops the tables it owns, so it
// is idempotent (mirrors content_load_db_it — the established DB-test pattern here).
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + seams;
// no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "ability_store.h"
#include "db_content_store.h"
#include "meridian/db/connection.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian;
namespace db = meridian::db;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

// The ability catalog tables the store reads, created WITHOUT foreign keys (the store
// only SELECTs columns; FK integrity is mcc's / the real DDL's concern). Columns +
// ENUM sets mirror schema/sql/world/30_ability.sql. DROP-then-CREATE so a rerun is
// clean.
void create_tables(db::Connection& c) {
    c.execute("DROP TABLE IF EXISTS ability_effect_stat_mod");
    c.execute("DROP TABLE IF EXISTS ability_effect");
    c.execute("DROP TABLE IF EXISTS ability");

    c.execute(
        "CREATE TABLE ability ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  school ENUM('physical','fire','frost','nature','shadow','holy','arcane') NOT NULL,"
        "  target ENUM('self','enemy','friendly') NOT NULL,"
        "  range_m FLOAT NOT NULL DEFAULT 5,"
        "  cast_time_ms INT UNSIGNED NULL DEFAULT 0, cast_channel_ms INT UNSIGNED NULL,"
        "  cooldown_ms INT UNSIGNED NOT NULL DEFAULT 0, triggers_gcd BOOLEAN NOT NULL DEFAULT TRUE,"
        "  resource_type ENUM('mana','rage','energy') NULL, resource_amount INT UNSIGNED NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE ability_effect ("
        "  ability_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  kind ENUM('damage','heal','aura','threat') NOT NULL,"
        "  amount_min INT UNSIGNED NULL, amount_max INT UNSIGNED NULL, coefficient FLOAT NULL,"
        "  threat_amount INT NULL, duration_ms INT UNSIGNED NULL,"
        "  max_stacks SMALLINT UNSIGNED NULL DEFAULT 1,"
        "  periodic_kind ENUM('damage','heal') NULL,"
        "  periodic_amount_min INT UNSIGNED NULL, periodic_amount_max INT UNSIGNED NULL,"
        "  periodic_tick_ms INT UNSIGNED NULL,"
        "  PRIMARY KEY (ability_id, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE ability_effect_stat_mod ("
        "  ability_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  stat ENUM('strength','agility','stamina','intellect','spirit') NOT NULL,"
        "  amount INT NOT NULL,"
        "  PRIMARY KEY (ability_id, ordinal, stat)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void drop_tables(db::Connection& c) {
    c.execute("DROP TABLE IF EXISTS ability_effect_stat_mod");
    c.execute("DROP TABLE IF EXISTS ability_effect");
    c.execute("DROP TABLE IF EXISTS ability");
}

// Seed the authored ids 1 (minor_healing) + 2 (pickaxe_slam) EXACTLY as mcc emit-sql
// produces them for content/core (tools/mcc/golden/world.sql), plus a synthetic buff
// (id 100) whose aura carries a stat_mod to exercise the stat_mod loader.
void seed_fixture(db::Connection& c) {
    // ability 1 — Minor Healing: holy, self, instant (cast 0), NO GCD, no resource,
    // 60 s cooldown. ability 2 — Pickaxe Slam: physical, enemy, 5 m, instant, GCD,
    // 20 energy, 10 s cooldown.
    c.execute(
        "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
        "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount) "
        "VALUES (1, 'Minor Healing', 'holy', 'self', 0, 0, NULL, 60000, FALSE, NULL, NULL)");
    c.execute(
        "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
        "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount) "
        "VALUES (2, 'Pickaxe Slam', 'physical', 'enemy', 5, 0, NULL, 10000, TRUE, 'energy', 20)");
    // Synthetic buff (clearly not authored content — high id) with an aura stat_mod,
    // to cover the stat_mod loader branch the authored abilities don't exercise.
    c.execute(
        "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
        "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount) "
        "VALUES (100, 'Test Vigor', 'nature', 'friendly', 30, 1500, NULL, 0, TRUE, 'mana', 15)");

    // effects: 1 -> heal 40-60; 2 -> damage 8-13 then a bleed aura (9 s, periodic
    // damage 2-3 every 3 s); 100 -> a pure stat-mod aura (30 s, no periodic tick).
    c.execute(
        "INSERT INTO ability_effect (ability_id, ordinal, kind, amount_min, amount_max, "
        "coefficient, threat_amount, duration_ms, max_stacks, periodic_kind, "
        "periodic_amount_min, periodic_amount_max, periodic_tick_ms) "
        "VALUES (1, 0, 'heal', 40, 60, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
    c.execute(
        "INSERT INTO ability_effect (ability_id, ordinal, kind, amount_min, amount_max, "
        "coefficient, threat_amount, duration_ms, max_stacks, periodic_kind, "
        "periodic_amount_min, periodic_amount_max, periodic_tick_ms) "
        "VALUES (2, 0, 'damage', 8, 13, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");
    c.execute(
        "INSERT INTO ability_effect (ability_id, ordinal, kind, amount_min, amount_max, "
        "coefficient, threat_amount, duration_ms, max_stacks, periodic_kind, "
        "periodic_amount_min, periodic_amount_max, periodic_tick_ms) "
        "VALUES (2, 1, 'aura', NULL, NULL, NULL, NULL, 9000, NULL, 'damage', 2, 3, 3000)");
    c.execute(
        "INSERT INTO ability_effect (ability_id, ordinal, kind, amount_min, amount_max, "
        "coefficient, threat_amount, duration_ms, max_stacks, periodic_kind, "
        "periodic_amount_min, periodic_amount_max, periodic_tick_ms) "
        "VALUES (100, 0, 'aura', NULL, NULL, NULL, NULL, 30000, 2, NULL, NULL, NULL, NULL)");

    // stat_mod for the synthetic buff's aura: +7 stamina.
    c.execute(
        "INSERT INTO ability_effect_stat_mod (ability_id, ordinal, stat, amount) "
        "VALUES (100, 0, 'stamina', 7)");
}

}  // namespace

int main() {
    std::printf("worldd world-DB ability-catalog load DB-backed integration test (#481)\n");

    db::ConnectParams p;
    bool configured = false;
    auto pick = [&](const char* world_key, const char* fallback_key) -> const char* {
        if (const char* v = env(world_key)) return v;
        return env(fallback_key);
    };
    if (const char* sk = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        p.unix_socket = sk; configured = true;
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
        std::printf("SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured — "
                    "ability-load checks skipped (set MERIDIAN_WORLDDB_HOST + "
                    "MERIDIAN_WORLDDB_USER, or reuse MERIDIAN_DB_*)\n");
        return 0;
    }

    // ---- REPRODUCTION (no DB needed) ---------------------------------------------
    // The placeholder store is what the live cast path used before this fix. It does
    // NOT contain the authored ids, so the CAST_REQUEST handler's find() misses and it
    // answers UNKNOWN_ABILITY — the exact bug. (This runs unconditionally so the
    // reproduction is asserted even under a DB, documenting WHY casting was broken.)
    {
        const worldd::AbilityStore placeholder = worldd::load_placeholder_ability_store();
        check("REPRO: placeholder store does NOT know authored id 1 (minor_healing)",
              placeholder.find(1) == nullptr);
        check("REPRO: placeholder store does NOT know authored id 2 (pickaxe_slam)",
              placeholder.find(2) == nullptr);
        check("REPRO: placeholder store is non-empty (its own synthetic ids)",
              !placeholder.empty());
    }

    try {
        db::Connection conn(p);
        create_tables(conn);
        seed_fixture(conn);

        // The boot load path: read the authored ability catalog from the world DB.
        const worldd::AbilityStore store = worldd::load_db_ability_store(conn);

        check("DB store loaded 3 abilities", store.size() == 3);

        // ---- FIX: authored id 1 (minor_healing) now RESOLVES (not UNKNOWN_ABILITY) --
        {
            const worldd::Ability* heal = store.find(1);
            check("FIX: authored id 1 resolves (was UNKNOWN_ABILITY under placeholder)",
                  heal != nullptr);
            if (heal) {
                check("id 1 name Minor Healing", heal->name == "Minor Healing");
                check("id 1 school holy", heal->school == worldd::School::kHoly);
                check("id 1 target self", heal->target == worldd::TargetKind::kSelf);
                check("id 1 instant (cast_time_ms 0)", heal->cast_time_ms == 0);
                check("id 1 off the GCD (triggers_gcd false)", heal->triggers_gcd == false);
                check("id 1 free (resource none, amount 0)",
                      heal->resource_type == worldd::AbilityResourceType::kNone &&
                          heal->resource_amount == 0);
                check("id 1 cooldown 60000 ms", heal->cooldown_ms == 60000);
                check("id 1 one heal effect 40-60",
                      heal->effects.size() == 1 &&
                          heal->effects[0].kind == worldd::EffectKind::kHeal &&
                          heal->effects[0].amount_min == 40 &&
                          heal->effects[0].amount_max == 60);
            }
        }

        // ---- FIX: authored id 2 (pickaxe_slam) resolves with damage + bleed aura ----
        {
            const worldd::Ability* slam = store.find(2);
            check("FIX: authored id 2 resolves", slam != nullptr);
            if (slam) {
                check("id 2 name Pickaxe Slam", slam->name == "Pickaxe Slam");
                check("id 2 school physical", slam->school == worldd::School::kPhysical);
                check("id 2 target enemy", slam->target == worldd::TargetKind::kEnemy);
                check("id 2 range 5 m", slam->range_m == 5.0f);
                check("id 2 energy cost 20",
                      slam->resource_type == worldd::AbilityResourceType::kEnergy &&
                          slam->resource_amount == 20);
                check("id 2 two effects (damage + aura)", slam->effects.size() == 2);
                if (slam->effects.size() == 2) {
                    check("id 2 effect 0 damage 8-13",
                          slam->effects[0].kind == worldd::EffectKind::kDamage &&
                              slam->effects[0].amount_min == 8 &&
                              slam->effects[0].amount_max == 13);
                    const worldd::AbilityEffect& aura = slam->effects[1];
                    check("id 2 effect 1 bleed aura: 9 s, periodic damage 2-3 @ 3 s",
                          aura.kind == worldd::EffectKind::kAura &&
                              aura.duration_ms == 9000 &&
                              aura.periodic_kind == worldd::PeriodicKind::kDamage &&
                              aura.periodic_amount_min == 2 &&
                              aura.periodic_amount_max == 3 &&
                              aura.periodic_tick_ms == 3000);
                }
            }
        }

        // ---- aura stat_mods load (synthetic buff id 100) ---------------------------
        {
            const worldd::Ability* buff = store.find(100);
            check("buff id 100 resolves", buff != nullptr);
            if (buff && buff->effects.size() == 1) {
                const worldd::AbilityEffect& aura = buff->effects[0];
                check("id 100 aura: 30 s, 2 stacks, no periodic",
                      aura.kind == worldd::EffectKind::kAura && aura.duration_ms == 30000 &&
                          aura.max_stacks == 2 &&
                          aura.periodic_kind == worldd::PeriodicKind::kNone);
                check("id 100 aura carries one stat_mod: +7 stamina",
                      aura.stat_mods.size() == 1 &&
                          aura.stat_mods[0].stat == worldd::StatKey::kStamina &&
                          aura.stat_mods[0].amount == 7);
            } else {
                check("id 100 has exactly one aura effect", false);
            }
        }

        // ---- PRESERVED: an unknown id still misses -> UNKNOWN_ABILITY ---------------
        check("PRESERVED: an id absent from the catalog find()-misses (UNKNOWN_ABILITY)",
              store.find(999) == nullptr);
        check("PRESERVED: the placeholder synthetic id is NOT in the DB store",
              store.find(worldd::kPlaceholderHealId) == nullptr);

        drop_tables(conn);
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all ability-load checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d ability-load check(s) failed\n", g_fail);
    return 1;
}
