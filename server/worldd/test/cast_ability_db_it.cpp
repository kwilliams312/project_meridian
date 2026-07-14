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
// `ability` table (carrying the effects[] recipe in effects_json, SP2.1) and mcc
// emits it.
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
#include "combat_unit.h"
#include "creature_ai.h"
#include "db_content_store.h"
#include "map_tick.h"
#include "movement_validation.h"
#include "meridian/db/connection.h"

#include <cmath>
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
    c.execute("DROP TABLE IF EXISTS ability");

    // SP2.1 (#691): the effect palette rides in the generic effects_json column;
    // the per-kind ability_effect / ability_effect_stat_mod child tables are gone.
    // Columns mirror schema/sql/world/30_ability.sql.
    c.execute(
        "CREATE TABLE ability ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  school ENUM('physical','fire','frost','nature','shadow','holy','arcane') NOT NULL,"
        "  target ENUM('self','enemy','friendly') NOT NULL,"
        "  range_m FLOAT NOT NULL DEFAULT 5,"
        "  cast_time_ms INT UNSIGNED NULL DEFAULT 0, cast_channel_ms INT UNSIGNED NULL,"
        "  cooldown_ms INT UNSIGNED NOT NULL DEFAULT 0, triggers_gcd BOOLEAN NOT NULL DEFAULT TRUE,"
        "  resource_type ENUM('mana','rage','energy') NULL, resource_amount INT UNSIGNED NULL,"
        "  effects_json JSON NOT NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void drop_tables(db::Connection& c) {
    c.execute("DROP TABLE IF EXISTS ability");
}

// Seed the authored ids 1 (minor_healing) + 2 (pickaxe_slam) EXACTLY as mcc emit-sql
// produces them for content/core (tools/mcc/golden/world.sql), plus a synthetic buff
// (id 100) whose aura carries a stat_mod to exercise the stat_mod loader. Effects are
// the canonical effects_json payload mcc emits (keys sorted; intRange as {min,max}).
void seed_fixture(db::Connection& c) {
    // ability 1 — Minor Healing: holy, self, instant (cast 0), NO GCD, no resource,
    // 60 s cooldown; effects: heal 40-60.
    c.execute(
        "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
        "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount, effects_json) "
        "VALUES (1, 'Minor Healing', 'holy', 'self', 0, 0, NULL, 60000, FALSE, NULL, NULL,"
        "        '[{\"amount\":{\"max\":60,\"min\":40},\"kind\":\"heal\"}]')");
    // ability 2 — Pickaxe Slam: physical, enemy, 5 m, instant, GCD, 20 energy, 10 s
    // cooldown; effects: damage 8-13 then a bleed aura (9 s, periodic damage 2-3 @ 3 s).
    c.execute(
        "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
        "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount, effects_json) "
        "VALUES (2, 'Pickaxe Slam', 'physical', 'enemy', 5, 0, NULL, 10000, TRUE, 'energy', 20,"
        "        '[{\"amount\":{\"max\":13,\"min\":8},\"kind\":\"damage\"},"
        "{\"duration_ms\":9000,\"kind\":\"aura\",\"periodic\":{\"amount\":{\"max\":3,\"min\":2},"
        "\"kind\":\"damage\",\"tick_ms\":3000}}]')");
    // Synthetic buff (clearly not authored content — high id) with a pure stat-mod
    // aura (30 s, 2 stacks, no periodic) carrying +7 stamina — covers the stat_mod
    // loader branch the authored abilities don't exercise.
    c.execute(
        "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
        "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount, effects_json) "
        "VALUES (100, 'Test Vigor', 'nature', 'friendly', 30, 1500, NULL, 0, TRUE, 'mana', 15,"
        "        '[{\"duration_ms\":30000,\"kind\":\"aura\",\"max_stacks\":2,"
        "\"stat_mods\":[{\"amount\":7,\"stat\":\"stamina\"}]}]')");

    // ── SP2.2 full-palette fixtures (#692) ──────────────────────────────────
    // One synthetic ability per NEW EffectKind, its effects_json written in the
    // canonical form mcc emits (object keys ascending; intRange as {max,min}). These
    // are clearly-synthetic ids (200+), NOT authored content — they exist to prove
    // db_content_store materializes EACH new palette kind into the right runtime
    // AbilityEffect variant with the right fields (LOAD/representation; #693 executes).
    auto ins = [&](int id, const char* name, const char* school, const char* target,
                   const char* res_type, const char* effects_json) {
        // resource_type NULL when res_type is empty; resource_amount fixed 10 otherwise.
        std::string sql =
            "INSERT INTO ability (id, name, school, target, range_m, cast_time_ms, "
            "cast_channel_ms, cooldown_ms, triggers_gcd, resource_type, resource_amount, "
            "effects_json) VALUES (";
        sql += std::to_string(id);
        sql += ", '"; sql += name; sql += "', '"; sql += school; sql += "', '";
        sql += target; sql += "', 30, 0, NULL, 0, TRUE, ";
        if (res_type[0] == '\0') sql += "NULL, NULL, '";
        else { sql += "'"; sql += res_type; sql += "', 10, '"; }
        sql += effects_json;
        sql += "')";
        c.execute(sql);
    };

    // 200 dot — per-tick 4-6 over 9 s @ 3 s, 3 stacks, coefficient 0.1.
    ins(200, "Test Dot", "fire", "enemy", "mana",
        "[{\"amount\":{\"max\":6,\"min\":4},\"coefficient\":0.1,\"duration_ms\":9000,"
        "\"kind\":\"dot\",\"max_stacks\":3,\"tick_ms\":3000}]");
    // 201 hot — per-tick 5-8 over 12 s @ 3 s (max_stacks/coefficient default).
    ins(201, "Test Hot", "holy", "friendly", "mana",
        "[{\"amount\":{\"max\":8,\"min\":5},\"duration_ms\":12000,\"kind\":\"hot\","
        "\"tick_ms\":3000}]");
    // 202 buff — +8 stamina (flat default) for 12 s.
    ins(202, "Test Buff", "physical", "self", "",
        "[{\"amount\":8,\"attribute\":\"core:attribute.stamina\",\"duration_ms\":12000,"
        "\"kind\":\"buff\"}]");
    // 203 debuff — -5 strength percent, 8 s, 2 stacks.
    ins(203, "Test Debuff", "shadow", "enemy", "mana",
        "[{\"amount\":-5,\"attribute\":\"core:attribute.strength\",\"duration_ms\":8000,"
        "\"kind\":\"debuff\",\"max_stacks\":2,\"modifier\":\"percent\"}]");
    // 204 shield — absorb 100-150 for 10 s, coefficient 0.5.
    ins(204, "Test Shield", "holy", "self", "mana",
        "[{\"amount\":{\"max\":150,\"min\":100},\"coefficient\":0.5,\"duration_ms\":10000,"
        "\"kind\":\"shield\"}]");
    // 205 cc — root for 4 s.
    ins(205, "Test Root", "nature", "enemy", "mana",
        "[{\"duration_ms\":4000,\"kind\":\"cc\",\"type\":\"root\"}]");
    // 206 resource — grant 200 mana.
    ins(206, "Test Mana Font", "arcane", "self", "",
        "[{\"amount\":200,\"kind\":\"resource\",\"operation\":\"grant\",\"pool\":\"mana\"}]");
    // 207 movement — knockback 8 m.
    ins(207, "Test Knockback", "physical", "enemy", "rage",
        "[{\"distance_m\":8,\"kind\":\"movement\",\"motion\":\"knockback\"}]");
    // 208 summon — 2 spirit wolves for 30 s (npc ref kept verbatim; resolved by #693).
    ins(208, "Test Summon", "nature", "self", "mana",
        "[{\"count\":2,\"duration_ms\":30000,\"kind\":\"summon\","
        "\"npc\":\"core:npc.spirit_wolf\"}]");
    // 209 boss slam — an ORDERED multi-primitive: damage → dot → debuff → cc → movement.
    ins(209, "Test Boss Slam", "physical", "enemy", "rage",
        "[{\"amount\":{\"max\":20,\"min\":15},\"kind\":\"damage\"},"
        "{\"amount\":{\"max\":3,\"min\":2},\"duration_ms\":6000,\"kind\":\"dot\",\"tick_ms\":2000},"
        "{\"amount\":-10,\"attribute\":\"core:attribute.agility\",\"duration_ms\":6000,\"kind\":\"debuff\"},"
        "{\"duration_ms\":2000,\"kind\":\"cc\",\"type\":\"stun\"},"
        "{\"distance_m\":5,\"kind\":\"movement\",\"motion\":\"knockback\"}]");
    // 210 forward-compat — an UNKNOWN future kind is SKIPPED (never crash), the known
    // damage effect still loads. Proves the defensive skip invariant is preserved.
    ins(210, "Test Unknown Skip", "physical", "enemy", "mana",
        "[{\"amount\":{\"max\":13,\"min\":8},\"kind\":\"damage\"},"
        "{\"kind\":\"bogus_future_kind\",\"amount\":99}]");
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

        check("DB store loaded 14 abilities (3 base + 11 full-palette)", store.size() == 14);

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

        // ======================================================================
        // SP2.2 FULL PALETTE (#692) — each new EffectKind deserializes correctly.
        // ======================================================================
        // These prove the acceptance bar for #692: the effects_json payload for
        // every Tier-1 primitive materializes into the right runtime AbilityEffect
        // variant with the right fields (LOAD-time; combat execution is #693).
        auto only_effect = [&](int id) -> const worldd::AbilityEffect* {
            const worldd::Ability* a = store.find(static_cast<worldd::AbilityId>(id));
            if (a == nullptr) { check("full-palette id resolves", false); return nullptr; }
            if (a->effects.size() != 1) {
                check("full-palette id has exactly one effect", false);
                return nullptr;
            }
            return &a->effects[0];
        };

        // 200 dot — per-tick 4-6 over 9 s @ 3 s, 3 stacks, coefficient 0.1.
        if (const worldd::AbilityEffect* e = only_effect(200)) {
            check("id 200 kDot: amount 4-6, dur 9 s, tick 3 s, 3 stacks, coeff 0.1",
                  e->kind == worldd::EffectKind::kDot && e->amount_min == 4 &&
                      e->amount_max == 6 && e->duration_ms == 9000 && e->tick_ms == 3000 &&
                      e->max_stacks == 3 && e->coefficient > 0.09f && e->coefficient < 0.11f);
        }
        // 201 hot — per-tick 5-8 over 12 s @ 3 s (defaults elsewhere).
        if (const worldd::AbilityEffect* e = only_effect(201)) {
            check("id 201 kHot: amount 5-8, dur 12 s, tick 3 s, default 1 stack",
                  e->kind == worldd::EffectKind::kHot && e->amount_min == 5 &&
                      e->amount_max == 8 && e->duration_ms == 12000 && e->tick_ms == 3000 &&
                      e->max_stacks == 1);
        }
        // 202 buff — +8 stamina flat (default) for 12 s.
        if (const worldd::AbilityEffect* e = only_effect(202)) {
            check("id 202 kBuff: +8 core:attribute.stamina, flat, 12 s",
                  e->kind == worldd::EffectKind::kBuff &&
                      e->attribute == "core:attribute.stamina" && e->attribute_amount == 8 &&
                      e->attribute_modifier == worldd::AttributeModifier::kFlat &&
                      e->duration_ms == 12000);
        }
        // 203 debuff — -5 strength percent, 8 s, 2 stacks.
        if (const worldd::AbilityEffect* e = only_effect(203)) {
            check("id 203 kDebuff: -5 core:attribute.strength, percent, 8 s, 2 stacks",
                  e->kind == worldd::EffectKind::kDebuff &&
                      e->attribute == "core:attribute.strength" && e->attribute_amount == -5 &&
                      e->attribute_modifier == worldd::AttributeModifier::kPercent &&
                      e->duration_ms == 8000 && e->max_stacks == 2);
        }
        // 204 shield — absorb 100-150 for 10 s, coefficient 0.5.
        if (const worldd::AbilityEffect* e = only_effect(204)) {
            check("id 204 kShield: absorb 100-150, 10 s, coeff 0.5",
                  e->kind == worldd::EffectKind::kShield && e->amount_min == 100 &&
                      e->amount_max == 150 && e->duration_ms == 10000 &&
                      e->coefficient > 0.49f && e->coefficient < 0.51f);
        }
        // 205 cc — root for 4 s.
        if (const worldd::AbilityEffect* e = only_effect(205)) {
            check("id 205 kCc: root, 4 s",
                  e->kind == worldd::EffectKind::kCc &&
                      e->cc_kind == worldd::CrowdControlKind::kRoot && e->duration_ms == 4000);
        }
        // 206 resource — grant 200 mana.
        if (const worldd::AbilityEffect* e = only_effect(206)) {
            check("id 206 kResource: grant 200 mana",
                  e->kind == worldd::EffectKind::kResource &&
                      e->resource_pool == worldd::ResourcePool::kMana &&
                      e->resource_op == worldd::ResourceOp::kGrant &&
                      e->resource_amount == 200);
        }
        // 207 movement — knockback 8 m.
        if (const worldd::AbilityEffect* e = only_effect(207)) {
            check("id 207 kMovement: knockback 8 m",
                  e->kind == worldd::EffectKind::kMovement &&
                      e->movement_motion == worldd::MovementMotion::kKnockback &&
                      e->distance_m > 7.9f && e->distance_m < 8.1f);
        }
        // 208 summon — 2 spirit wolves for 30 s (npc ref verbatim).
        if (const worldd::AbilityEffect* e = only_effect(208)) {
            check("id 208 kSummon: 2x core:npc.spirit_wolf, 30 s",
                  e->kind == worldd::EffectKind::kSummon &&
                      e->summon_npc == "core:npc.spirit_wolf" && e->summon_count == 2 &&
                      e->duration_ms == 30000);
        }
        // 209 boss slam — an ORDERED multi-primitive chain (order + mixed kinds).
        {
            const worldd::Ability* slam = store.find(209);
            check("id 209 boss slam resolves", slam != nullptr);
            if (slam) {
                check("id 209 has 5 ordered effects", slam->effects.size() == 5);
                if (slam->effects.size() == 5) {
                    check("id 209 [0] damage 15-20",
                          slam->effects[0].kind == worldd::EffectKind::kDamage &&
                              slam->effects[0].amount_min == 15 &&
                              slam->effects[0].amount_max == 20);
                    check("id 209 [1] dot 2-3 @ 2 s / 6 s",
                          slam->effects[1].kind == worldd::EffectKind::kDot &&
                              slam->effects[1].amount_min == 2 &&
                              slam->effects[1].tick_ms == 2000 &&
                              slam->effects[1].duration_ms == 6000);
                    check("id 209 [2] debuff -10 agility",
                          slam->effects[2].kind == worldd::EffectKind::kDebuff &&
                              slam->effects[2].attribute == "core:attribute.agility" &&
                              slam->effects[2].attribute_amount == -10);
                    check("id 209 [3] cc stun 2 s",
                          slam->effects[3].kind == worldd::EffectKind::kCc &&
                              slam->effects[3].cc_kind == worldd::CrowdControlKind::kStun &&
                              slam->effects[3].duration_ms == 2000);
                    check("id 209 [4] movement knockback 5 m",
                          slam->effects[4].kind == worldd::EffectKind::kMovement &&
                              slam->effects[4].movement_motion ==
                                  worldd::MovementMotion::kKnockback);
                }
            }
        }
        // 210 forward-compat — an UNKNOWN kind is SKIPPED, the known damage survives.
        {
            const worldd::Ability* a = store.find(210);
            check("id 210 (unknown-kind ability) still loads", a != nullptr);
            if (a) {
                check("id 210 unknown future kind SKIPPED — only the damage effect kept",
                      a->effects.size() == 1 &&
                          a->effects[0].kind == worldd::EffectKind::kDamage &&
                          a->effects[0].amount_min == 8 && a->effects[0].amount_max == 13);
            }
        }

        // ======================================================================
        // SP2.3 RUNTIME EXECUTION (#693) — the DB-backed effects RESOLVE end-to-end.
        // ======================================================================
        // The MANDATORY DB-backed runtime proof: cast the DB-LOADED abilities through
        // the ability engine (MapTick) and observe each new primitive's server-
        // authoritative effect EXECUTE — not mocked. This proves the whole pipeline:
        // effects_json (MariaDB) → runtime AbilityEffect (SP2.2) → engine execution
        // (SP2.3). A fixed CombatRng seed makes every roll deterministic.
        {
            namespace w = meridian::worldd;
            namespace mc = meridian::worldd::movement;
            auto at = [](float x, float y) {
                w::Position p; p.x = x; p.y = y; p.z = mc::kFlatGroundZ; return p;
            };
            auto pstats = [](std::uint32_t hp, std::uint32_t mana) {
                w::UnitStats s; s.level = 5; s.max_health = hp;
                s.resource_type = w::ResourceType::kMana; s.max_resource = mana;
                s.faction = w::Faction::kPlayer; return s;
            };
            auto cmob = [&](w::Position home) {
                w::CreatureSpawnDef d; d.template_id = 1; d.level = 5;
                d.faction = w::Faction::kHostile; d.home = home; d.aggro_base_radius = 0;
                d.leash_radius = 100000; d.respawn_ms = 999999; d.move_speed = 0;
                d.patrol_mode = w::PatrolMode::kStationary; return d;
            };

            // id 200 dot (per-tick 4-6 @ 3 s over 9 s) on a creature → periodic damage.
            {
                w::MapTick mt(store, 0xDB2003ULL, /*dt=*/3000);
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                const w::ObjectGuid c = mt.add_creature(cmob(at(2, 0)));
                mt.ai().creature(c)->set_max_health(500);
                const std::uint32_t hp0 = mt.ai().creature(c)->health();
                mt.enqueue_cast(w::AbilityUseCmd{p, 200, c});
                mt.advance(4);  // 3 ticks over 9 s @ dt 3 s
                check("RUNTIME id 200 dot dealt periodic damage from the DB effect",
                      mt.ai().creature(c)->health() < hp0);
            }
            // id 201 hot (per-tick 5-8 @ 3 s over 12 s) on a wounded self → periodic heal.
            {
                w::MapTick mt(store, 0xDB2011ULL, /*dt=*/3000);
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                mt.unit_for_guid(p)->apply_damage(500);  // 500/1000
                const std::uint32_t hp0 = mt.unit_for_guid(p)->health();
                mt.enqueue_cast(w::AbilityUseCmd{p, 201, p});
                mt.advance(3);
                check("RUNTIME id 201 hot healed periodic HP from the DB effect",
                      mt.unit_for_guid(p)->health() > hp0);
            }
            // id 204 shield (absorb 100-150) on self → absorb pool granted; soaks a hit.
            {
                w::MapTick mt(store, 0xDB2044ULL, /*dt=*/1000);
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                mt.enqueue_cast(w::AbilityUseCmd{p, 204, p});
                mt.advance();
                const std::uint32_t shield = mt.unit_for_guid(p)->absorb();
                check("RUNTIME id 204 shield granted an absorb pool (>=100)", shield >= 100);
                const w::DamageResult dr = mt.unit_for_guid(p)->apply_damage(50);
                check("RUNTIME id 204 shield absorbed a 50 hit (0 HP lost)",
                      dr.absorbed == 50 && dr.applied == 0);
            }
            // id 206 resource (grant 200 mana) on self → the pool refills.
            {
                w::MapTick mt(store, 0xDB2066ULL, /*dt=*/1000);
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                mt.unit_for_guid(p)->spend_resource(900);  // 1000 -> 100
                mt.enqueue_cast(w::AbilityUseCmd{p, 206, p});
                mt.advance();
                check("RUNTIME id 206 resource grant refilled 200 mana (100 -> 300)",
                      mt.unit_for_guid(p)->resource() == 300);
            }
            // id 207 movement (knockback 8 m) on a creature → server-side displacement.
            {
                w::MapTick mt(store, 0xDB2077ULL, /*dt=*/1000);
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                const w::ObjectGuid c = mt.add_creature(cmob(at(10, 0)));
                mt.enqueue_cast(w::AbilityUseCmd{p, 207, c});
                mt.advance();
                check("RUNTIME id 207 knockback displaced the target to ~x=18",
                      std::lround(mt.ai().creature(c)->position().x) == 18);
            }
            // id 208 summon (2x core:npc.spirit_wolf for 30 s) → resolver spawns them.
            {
                w::MapTick mt(store, 0xDB2088ULL, /*dt=*/1000);
                mt.set_summon_resolver([](const std::string& ref, w::CreatureSpawnDef& out) {
                    if (ref != "core:npc.spirit_wolf") return false;
                    out.template_id = 9001; out.level = 3; out.faction = w::Faction::kFriendly;
                    return true;
                });
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                const std::size_t before = mt.ai().size();
                mt.enqueue_cast(w::AbilityUseCmd{p, 208, p});
                mt.advance();
                check("RUNTIME id 208 summon spawned 2 creatures from the DB npc ref",
                      mt.ai().size() == before + 2);
            }
            // id 209 boss slam — the ORDERED multi-primitive chain (damage → dot →
            // debuff → cc → knockback) executes in one cast: observable damage + move.
            {
                w::MapTick mt(store, 0xDB2099ULL, /*dt=*/2000);
                const w::ObjectGuid p = mt.add_player(1, at(0, 0), pstats(1000, 1000));
                const w::ObjectGuid c = mt.add_creature(cmob(at(10, 0)));
                mt.ai().creature(c)->set_max_health(500);
                const std::uint32_t hp0 = mt.ai().creature(c)->health();
                mt.enqueue_cast(w::AbilityUseCmd{p, 209, c});
                mt.advance(4);  // apply + dot ticks (2-3 @ 2 s over 6 s)
                check("RUNTIME id 209 boss slam knockback displaced the target (x=15)",
                      std::lround(mt.ai().creature(c)->position().x) == 15);
                check("RUNTIME id 209 boss slam dealt damage (direct + dot)",
                      mt.ai().creature(c)->health() < hp0);
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
