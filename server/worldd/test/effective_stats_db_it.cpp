// SPDX-License-Identifier: Apache-2.0
//
// worldd — EFFECTIVE-STAT framework DB-backed integration test (SP2.4 #694, epic
// #690). The MANDATORY DB-backed proof (SP2 design §4, "the SP1.8 lesson"): a
// character's effective stats reflect base + class attribute_mods + race
// attribute_mods LOADED FROM A REAL MariaDB, a buff modifies the right effective
// attribute (FLAT and PERCENT), a DERIVED attribute (armor) computes correctly, and
// aura EXPIRY rolls the contribution back.
//
// It seeds the `attribute` / `class_attribute_mod` / `race_attribute_mod` tables
// (mirroring mcc emit-sql's output shape — the same rows scripts/dev/test.sh --db
// loads from the real content pack) into minimal no-FK tables — the established
// DB-test pattern in this repo (content_load_db_it, world_boot_db_test) — then reads
// them back with the REAL load_db_attributes into an AttributeCatalog, and drives the
// pure EffectiveStats + AuraContainer over that catalog. So the class/race mods, the
// primary/derived split, and the final effective value are all computed off data that
// round-tripped through MariaDB, not an in-memory fixture.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0)
// when neither is set — inert in the plain server ctest, real only under test.sh --db
// / the DB CI job. Creates + drops the tables it owns (idempotent).
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + the
// effective-stat framework; no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "ability_store.h"
#include "aura_container.h"
#include "combat_resolver.h"
#include "combat_unit.h"
#include "db_content_store.h"
#include "effective_stats.h"
#include "meridian/db/connection.h"
#include "movement_validation.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian;
using namespace meridian::worldd;
namespace db = meridian::db;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

const std::string kStr = "core:attribute.strength";
const std::string kAgi = "core:attribute.agility";
const std::string kSta = "core:attribute.stamina";
const std::string kArmor = "core:attribute.armor";  // derived

void create_tables(db::Connection& c) {
    for (const char* d : {"DROP TABLE IF EXISTS class_attribute_mod",
                          "DROP TABLE IF EXISTS race_attribute_mod",
                          "DROP TABLE IF EXISTS attribute"}) {
        c.execute(d);
    }
    // Columns mirror schema/sql/world/36_attribute.sql; no FKs (the loader only
    // SELECTs, like the other self-seeded content DB tests).
    c.execute(
        "CREATE TABLE attribute ("
        "  attr_ref VARCHAR(64) NOT NULL, content_id INT UNSIGNED NOT NULL,"
        "  name VARCHAR(64) NOT NULL, kind ENUM('primary','derived') NOT NULL,"
        "  PRIMARY KEY (attr_ref)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE class_attribute_mod ("
        "  class_roster_id TINYINT UNSIGNED NOT NULL, attr_ref VARCHAR(64) NOT NULL, value INT NOT NULL,"
        "  PRIMARY KEY (class_roster_id, attr_ref)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE race_attribute_mod ("
        "  race_roster_id TINYINT UNSIGNED NOT NULL, attr_ref VARCHAR(64) NOT NULL, value INT NOT NULL,"
        "  PRIMARY KEY (race_roster_id, attr_ref)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void drop_tables(db::Connection& c) {
    for (const char* d : {"DROP TABLE IF EXISTS class_attribute_mod",
                          "DROP TABLE IF EXISTS race_attribute_mod",
                          "DROP TABLE IF EXISTS attribute"}) {
        c.execute(d);
    }
}

// Seed the vocabulary + mods (mcc emit-sql's shape). The Vanguard (class 1) tunes
// strength +2 / stamina +1 (the seed pack), and a tuned race (race 2) tunes agility
// +1 and the DERIVED armor +5 — so BOTH the class and race layers, and a derived
// attribute, are exercised off real DB rows.
void seed(db::Connection& c) {
    c.execute("INSERT INTO attribute (attr_ref, content_id, name, kind) VALUES "
              "('core:attribute.strength', 132, 'Strength', 'primary'),"
              "('core:attribute.agility', 126, 'Agility', 'primary'),"
              "('core:attribute.stamina', 131, 'Stamina', 'primary'),"
              "('core:attribute.armor', 127, 'Armor', 'derived')");
    c.execute("INSERT INTO class_attribute_mod (class_roster_id, attr_ref, value) VALUES "
              "(1, 'core:attribute.strength', 2),"
              "(1, 'core:attribute.stamina', 1)");
    c.execute("INSERT INTO race_attribute_mod (race_roster_id, attr_ref, value) VALUES "
              "(2, 'core:attribute.agility', 1),"
              "(2, 'core:attribute.armor', 5)");
}

Unit make_unit() {
    UnitStats st;
    st.level = 10;
    st.max_health = 1000;
    return Unit{1, ObjectType::kPlayer, Position{0, 0, 0}, st};
}

// One buff effect (kBuff on `attr`, `amount` with `mod` flat/percent, 10s).
AbilityEffect buff_effect(const std::string& attr, std::int32_t amount,
                          AttributeModifier mod) {
    AbilityEffect e;
    e.kind = EffectKind::kBuff;
    e.duration_ms = 10'000;
    e.attribute = attr;
    e.attribute_amount = amount;
    e.attribute_modifier = mod;
    return e;
}

}  // namespace

int main() {
    std::printf("worldd effective-stat framework DB-backed test (#694)\n");

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
                    "effective-stat DB checks skipped\n");
        return 0;
    }

    try {
        db::Connection conn(p);
        create_tables(conn);
        seed(conn);

        // Load the attribute framework back out of the REAL DB (the boot load path).
        const AttributeCatalog cat = load_db_attributes(conn);
        check("catalog: 4 attributes loaded from DB", cat.attribute_count() == 4);
        check("catalog: 3 primary + 1 derived", cat.primary_count() == 3 && cat.derived_count() == 1);
        check("catalog: strength primary, armor derived",
              cat.is_primary(kStr) && !cat.is_primary(kArmor) &&
                  cat.find(kArmor) != nullptr);
        check("catalog: class 1 strength +2 (from DB)", cat.class_mod(1, kStr) == 2);
        check("catalog: class 1 stamina +1 (from DB)", cat.class_mod(1, kSta) == 1);
        check("catalog: race 2 agility +1 (from DB)", cat.race_mod(2, kAgi) == 1);
        check("catalog: race 2 armor +5 (from DB, derived)", cat.race_mod(2, kArmor) == 5);

        // A Vanguard (class 1) of the tuned race (race 2). Base primaries set by the
        // character; deriveds start at 0.
        EffectiveStats es(cat, /*race=*/2, /*class=*/1);
        es.set_base(kStr, 10);
        es.set_base(kAgi, 8);
        es.set_base(kSta, 5);

        // ---- base + class + race, all from MariaDB -----------------------------
        check("effective strength at rest = 10 + 2(class) = 12", es.static_value(kStr) == 12);
        check("effective agility at rest = 8 + 1(race) = 9", es.static_value(kAgi) == 9);
        check("effective stamina at rest = 5 + 1(class) = 6", es.static_value(kSta) == 6);
        check("effective armor at rest = 0 + 5(race) = 5 (derived)", es.static_value(kArmor) == 5);

        // ---- a buff modifies the right effective attribute (FLAT and PERCENT) ---
        Unit host = make_unit();
        AuraContainer auras{host};
        // no auras yet -> effective == at-rest.
        check("no auras: strength effective == 12", es.effective(kStr, auras) == 12);

        // A single ability: [0] strength FLAT +8, [1] strength PERCENT +50%,
        // [2] armor FLAT +10, [3] armor PERCENT +20%.
        Ability buff;
        buff.id = 7001;
        buff.effects.push_back(buff_effect(kStr, 8, AttributeModifier::kFlat));
        buff.effects.push_back(buff_effect(kStr, 5000, AttributeModifier::kPercent));
        buff.effects.push_back(buff_effect(kArmor, 10, AttributeModifier::kFlat));
        buff.effects.push_back(buff_effect(kArmor, 2000, AttributeModifier::kPercent));
        const std::size_t applied = auras.apply_ability_effects(buff, /*caster=*/42);
        check("4 buff effects applied to the container", applied == 4);

        // PRIMARY strength: (12 static + 8 flat) * 1.50 = 30.
        check("buffed strength = (12+8)*1.50 = 30", es.effective(kStr, auras) == 30);
        // The primary FLAT part also mirrors onto the StatKey layer (SP2.3 coherence).
        check("StatKey strength delta mirrors the +8 flat", auras.stat_delta(StatKey::kStrength) == 8);
        // DERIVED armor: (5 static + 10 flat) * 1.20 = 18 — a derived attribute
        // computes correctly through the same framework (the #694 point).
        check("buffed armor = (5+10)*1.20 = 18 (derived, flat+percent)",
              es.effective(kArmor, auras) == 18);
        // Breakdown makes the layers explicit.
        const EffectiveStat b = es.breakdown(kStr, auras);
        check("strength breakdown base 10", b.base == 10);
        check("strength breakdown flat_mods 2(class)+8(buff) = 10", b.flat_mods == 10);
        check("strength breakdown percent 5000", b.percent == 5000);
        check("strength breakdown value 30", b.value == 30);

        // ---- expiry rolls the contribution back --------------------------------
        CombatRng rng{0xABCDu};
        auras.tick(10'000, rng);
        check("all buff auras expired", auras.empty());
        check("strength effective back to at-rest 12", es.effective(kStr, auras) == 12);
        check("armor effective back to at-rest 5", es.effective(kArmor, auras) == 5);
        check("StatKey strength delta rolled back to 0", auras.stat_delta(StatKey::kStrength) == 0);

        drop_tables(conn);
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all effective-stat DB-backed checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d effective-stat DB-backed check(s) failed\n", g_fail);
    return 1;
}
