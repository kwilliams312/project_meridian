// SPDX-License-Identifier: Apache-2.0
//
// worldd — equip-gating + category-match + role/talent DB-backed integration test
// (SP2.7 #697, epic #690). The MANDATORY DB-backed proof (SP2 design §4, "the SP1.8
// lesson"): the class kernel's equip decision, the role hook, and the talent
// application all run over data that ROUND-TRIPPED THROUGH A REAL MariaDB.
//
// It seeds the `equip_type` / `class` / `class_usable_equip_type` / `class_role`
// tables (+ `talent*` + `attribute` + `item_template`) — mirroring mcc emit-sql's
// output shape, the same rows scripts/dev/test.sh --db loads from the real content
// pack — into minimal no-FK tables (the established DB-test pattern:
// effective_stats_db_it / content_load_db_it), then reads them back with the REAL
// load_db_equip_types / load_db_class_catalog / load_db_talents / DbTemplateStore and
// drives gate_equip + threat_multiplier + apply_talents over the loaded catalogs.
//
// PROVES:
//   * ACCEPT   — a Vanguard equips a plate chest (plate ∈ its usable_armor_types,
//                category matches slot) and an iron sword (one_hand ∈ usable_weapon).
//   * REJECT   — a Warden CANNOT equip the plate chest (plate ∉ {leather, mail}).
//   * REJECT   — a weapon-category item in an ARMOR slot is a category mismatch
//                (the closed SP1-deferred check), for either class.
//   * ROLE     — the Tank-role Vanguard amplifies threat (2.0x); the Warden does not.
//   * TALENT   — the Vanguard's talent tree grants an ability and a permanent passive
//                that raises the character's effective strength through the SP2.4
//                effective-stat framework.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0)
// when neither is set — inert in the plain server ctest, real only under test.sh --db
// / the DB CI job. Creates + drops the tables it owns (idempotent).
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + the
// class kernel; no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "class_kernel.h"
#include "db_content_store.h"
#include "effective_stats.h"
#include "meridian/db/connection.h"
#include "talent_catalog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian;
using namespace meridian::worldd;
namespace db = meridian::db;
namespace itm = meridian::items;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

// Item template ids the test seeds.
constexpr std::uint32_t kWardenChest = 102;   // plate, chest (armor)
constexpr std::uint32_t kIronSword = 114;     // one_hand, main_hand (weapon)
constexpr std::uint32_t kCursedBlade = 900;   // one_hand equip_type but a CHEST slot (mismatch)

void create_tables(db::Connection& c) {
    for (const char* d : {"DROP TABLE IF EXISTS equip_type",
                          "DROP TABLE IF EXISTS class",
                          "DROP TABLE IF EXISTS class_usable_equip_type",
                          "DROP TABLE IF EXISTS class_role",
                          "DROP TABLE IF EXISTS talent",
                          "DROP TABLE IF EXISTS talent_grant",
                          "DROP TABLE IF EXISTS talent_tree",
                          "DROP TABLE IF EXISTS talent_tree_tier",
                          "DROP TABLE IF EXISTS talent_tree_tier_talent",
                          "DROP TABLE IF EXISTS attribute",
                          "DROP TABLE IF EXISTS class_attribute_mod",
                          "DROP TABLE IF EXISTS race_attribute_mod",
                          "DROP TABLE IF EXISTS item_template",
                          "DROP TABLE IF EXISTS item_stat",
                          "DROP TABLE IF EXISTS race"}) {
        c.execute(d);
    }
    // Columns mirror schema/sql/world/{15_equip_type,35_roster,38_talent,36_attribute,
    // 20_item}.sql; no FKs (the loaders only SELECT, like the other DB content tests).
    c.execute("CREATE TABLE equip_type ("
              "  content_id INT UNSIGNED NOT NULL, equip_ref VARCHAR(96) NOT NULL,"
              "  name VARCHAR(64) NOT NULL, category ENUM('armor','weapon') NOT NULL,"
              "  slot_class VARCHAR(32) NULL, PRIMARY KEY (content_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class ("
              "  roster_id TINYINT UNSIGNED NOT NULL, content_id INT UNSIGNED NOT NULL,"
              "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
              "  talent_tree_id INT UNSIGNED NULL, PRIMARY KEY (roster_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class_usable_equip_type ("
              "  class_roster_id TINYINT UNSIGNED NOT NULL, equip_type_id INT UNSIGNED NOT NULL,"
              "  list ENUM('armor','weapon') NOT NULL,"
              "  PRIMARY KEY (class_roster_id, equip_type_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class_role ("
              "  class_roster_id TINYINT UNSIGNED NOT NULL,"
              "  role ENUM('healer','dps_melee','dps_ranged','tank') NOT NULL,"
              "  PRIMARY KEY (class_roster_id, role))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent ("
              "  content_id INT UNSIGNED NOT NULL, talent_ref VARCHAR(96) NOT NULL,"
              "  name VARCHAR(64) NOT NULL, rank_max SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
              "  PRIMARY KEY (content_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_grant ("
              "  talent_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
              "  kind ENUM('ability','buff','debuff') NOT NULL, ability_id INT UNSIGNED NULL,"
              "  attribute_ref VARCHAR(64) NULL, amount INT NULL,"
              "  modifier ENUM('flat','percent') NULL, duration_ms INT UNSIGNED NULL,"
              "  max_stacks SMALLINT UNSIGNED NULL, PRIMARY KEY (talent_id, ordinal))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_tree ("
              "  content_id INT UNSIGNED NOT NULL, tree_ref VARCHAR(96) NOT NULL,"
              "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
              "  PRIMARY KEY (content_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_tree_tier ("
              "  talent_tree_id INT UNSIGNED NOT NULL, tier_ordinal SMALLINT UNSIGNED NOT NULL,"
              "  required_points SMALLINT UNSIGNED NOT NULL,"
              "  PRIMARY KEY (talent_tree_id, tier_ordinal))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_tree_tier_talent ("
              "  talent_tree_id INT UNSIGNED NOT NULL, tier_ordinal SMALLINT UNSIGNED NOT NULL,"
              "  ordinal SMALLINT UNSIGNED NOT NULL, talent_id INT UNSIGNED NOT NULL,"
              "  PRIMARY KEY (talent_tree_id, tier_ordinal, ordinal))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE attribute ("
              "  attr_ref VARCHAR(64) NOT NULL, content_id INT UNSIGNED NOT NULL,"
              "  name VARCHAR(64) NOT NULL, kind ENUM('primary','derived') NOT NULL,"
              "  PRIMARY KEY (attr_ref))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class_attribute_mod ("
              "  class_roster_id TINYINT UNSIGNED NOT NULL, attr_ref VARCHAR(64) NOT NULL,"
              "  value INT NOT NULL, PRIMARY KEY (class_roster_id, attr_ref))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE race_attribute_mod ("
              "  race_roster_id TINYINT UNSIGNED NOT NULL, attr_ref VARCHAR(64) NOT NULL,"
              "  value INT NOT NULL, PRIMARY KEY (race_roster_id, attr_ref))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // Minimal item_template with exactly the columns DbTemplateStore SELECTs.
    c.execute("CREATE TABLE item_template ("
              "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
              "  item_class ENUM('weapon','armor','consumable','quest','trade_good','container') NOT NULL,"
              "  slot ENUM('head','shoulders','back','chest','wrist','hands','waist','legs','feet',"
              "            'neck','finger','trinket','main_hand','off_hand','two_hand','ranged','bag') NULL,"
              "  equip_type_id INT UNSIGNED NULL,"
              "  rarity ENUM('poor','common','uncommon','rare','epic','legendary') NOT NULL DEFAULT 'common',"
              "  required_level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
              "  item_level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
              "  is_unique BOOLEAN NOT NULL DEFAULT FALSE,"
              "  binding ENUM('none','on_pickup','on_equip') NOT NULL DEFAULT 'none',"
              "  stack_size SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
              "  weapon_damage_min INT UNSIGNED NULL, weapon_damage_max INT UNSIGNED NULL,"
              "  weapon_speed_ms INT UNSIGNED NULL, armor INT UNSIGNED NULL,"
              "  price_sell BIGINT UNSIGNED NULL, price_buy BIGINT UNSIGNED NULL,"
              "  PRIMARY KEY (id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // DbTemplateStore's ctor also SELECTs the item_stat child table — create it empty.
    c.execute("CREATE TABLE item_stat ("
              "  item_id INT UNSIGNED NOT NULL, stat VARCHAR(16) NOT NULL, amount INT NOT NULL,"
              "  PRIMARY KEY (item_id, stat))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void drop_tables(db::Connection& c) {
    for (const char* d : {"DROP TABLE IF EXISTS equip_type",
                          "DROP TABLE IF EXISTS class",
                          "DROP TABLE IF EXISTS class_usable_equip_type",
                          "DROP TABLE IF EXISTS class_role",
                          "DROP TABLE IF EXISTS talent",
                          "DROP TABLE IF EXISTS talent_grant",
                          "DROP TABLE IF EXISTS talent_tree",
                          "DROP TABLE IF EXISTS talent_tree_tier",
                          "DROP TABLE IF EXISTS talent_tree_tier_talent",
                          "DROP TABLE IF EXISTS attribute",
                          "DROP TABLE IF EXISTS class_attribute_mod",
                          "DROP TABLE IF EXISTS race_attribute_mod",
                          "DROP TABLE IF EXISTS item_template",
                          "DROP TABLE IF EXISTS item_stat"}) {
        c.execute(d);
    }
}

// Seed the seed-pack values (mirrors world.sql): equip_type catalog, the Vanguard
// (roster 1) + Warden (roster 3) classes with their usable types + roles, the
// vanguard_path talent tree, the attribute vocabulary, and three item templates.
void seed(db::Connection& c) {
    // equip_type: mail=120, one_hand=121, plate=122, staff=123, two_hand=124, leather=119.
    c.execute("INSERT INTO equip_type (content_id, equip_ref, name, category, slot_class) VALUES "
              "(119, 'core:equip_type.leather', 'Leather', 'armor', NULL),"
              "(120, 'core:equip_type.mail', 'Mail', 'armor', NULL),"
              "(121, 'core:equip_type.one_hand', 'One-Hand', 'weapon', 'main'),"
              "(122, 'core:equip_type.plate', 'Plate', 'armor', NULL),"
              "(123, 'core:equip_type.staff', 'Staff', 'weapon', 'two_hand'),"
              "(124, 'core:equip_type.two_hand', 'Two-Hand', 'weapon', 'two_hand')");
    c.execute("INSERT INTO class (roster_id, content_id, name, description, talent_tree_id) VALUES "
              "(1, 137, 'Vanguard', 'A front-line melee defender.', 143),"
              "(3, 138, 'Warden', 'A ranged hybrid.', NULL)");
    c.execute("INSERT INTO class_usable_equip_type (class_roster_id, equip_type_id, list) VALUES "
              "(1, 120, 'armor'), (1, 122, 'armor'), (1, 121, 'weapon'), (1, 124, 'weapon'),"
              "(3, 119, 'armor'), (3, 120, 'armor'), (3, 121, 'weapon'), (3, 123, 'weapon')");
    c.execute("INSERT INTO class_role (class_roster_id, role) VALUES "
              "(1, 'tank'), (3, 'dps_ranged'), (3, 'healer')");
    c.execute("INSERT INTO talent (content_id, talent_ref, name, rank_max) VALUES "
              "(141, 'core:talent.battle_fury', 'Battle Fury', 3),"
              "(142, 'core:talent.warding_grace', 'Warding Grace', 1)");
    c.execute("INSERT INTO talent_grant (talent_id, ordinal, kind, ability_id, attribute_ref, amount, modifier, duration_ms, max_stacks) VALUES "
              "(141, 0, 'ability', 133, NULL, NULL, NULL, NULL, NULL),"
              "(141, 1, 'buff', NULL, 'core:attribute.strength', 5, 'flat', NULL, 1),"
              "(142, 0, 'ability', 135, NULL, NULL, NULL, NULL, NULL),"
              "(142, 1, 'buff', NULL, 'core:attribute.intellect', 3, 'flat', NULL, 1)");
    c.execute("INSERT INTO talent_tree (content_id, tree_ref, name, description) VALUES "
              "(143, 'core:talent_tree.vanguard_path', 'Vanguard Path', 'The tree.')");
    c.execute("INSERT INTO talent_tree_tier (talent_tree_id, tier_ordinal, required_points) VALUES "
              "(143, 0, 0), (143, 1, 5)");
    c.execute("INSERT INTO talent_tree_tier_talent (talent_tree_id, tier_ordinal, ordinal, talent_id) VALUES "
              "(143, 0, 0, 141), (143, 1, 0, 142)");
    c.execute("INSERT INTO attribute (attr_ref, content_id, name, kind) VALUES "
              "('core:attribute.strength', 132, 'Strength', 'primary'),"
              "('core:attribute.intellect', 129, 'Intellect', 'primary')");
    // The Vanguard (class 1) tunes strength +2 (a seed class_attribute_mod).
    c.execute("INSERT INTO class_attribute_mod (class_roster_id, attr_ref, value) VALUES "
              "(1, 'core:attribute.strength', 2)");
    // item_template: a plate chest (armor), an iron sword (one_hand weapon), and a
    // MALFORMED item — a one_hand (weapon) equip_type wrongly slotted in a CHEST slot
    // (to exercise the runtime category-mismatch rejection).
    c.execute("INSERT INTO item_template (id, name, item_class, slot, equip_type_id, rarity, required_level, item_level, is_unique, binding, stack_size, armor) VALUES "
              "(102, 'Warden''s Cuirass', 'armor', 'chest', 122, 'rare', 10, 14, FALSE, 'on_equip', 1, 80)");
    c.execute("INSERT INTO item_template (id, name, item_class, slot, equip_type_id, rarity, required_level, item_level, is_unique, binding, stack_size, weapon_damage_min, weapon_damage_max, weapon_speed_ms) VALUES "
              "(114, 'Iron Sword', 'weapon', 'main_hand', 121, 'common', 1, 5, FALSE, 'none', 1, 6, 11, 2400)");
    c.execute("INSERT INTO item_template (id, name, item_class, slot, equip_type_id, rarity) VALUES "
              "(900, 'Cursed Plate Blade', 'armor', 'chest', 121, 'common')");
}

}  // namespace

int main() {
    std::printf("worldd equip-gating + role/talent DB-backed test (#697)\n");

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
                    "equip-gating DB checks skipped\n");
        return 0;
    }

    try {
        db::Connection conn(p);
        create_tables(conn);
        seed(conn);

        // ---- Load every catalog back out of the REAL DB (the boot load path) ----
        const EquipTypeCatalog equip_types = load_db_equip_types(conn);
        const ClassCatalog classes = load_db_class_catalog(conn);
        const TalentCatalog talents = load_db_talents(conn);
        const AttributeCatalog attrs = load_db_attributes(conn);
        DbTemplateStore items(conn);

        check("6 equip_types loaded from DB", equip_types.size() == 6);
        check("plate is an armor category (from DB)",
              equip_types.find(122) != nullptr &&
                  equip_types.find(122)->category == EquipCategory::kArmor);
        check("one_hand is a weapon category (from DB)",
              equip_types.find(121) != nullptr &&
                  equip_types.find(121)->category == EquipCategory::kWeapon);
        check("2 classes loaded from DB", classes.size() == 2);

        const ClassRecord* vg = classes.find(1);  // Vanguard
        const ClassRecord* wd = classes.find(3);  // Warden
        check("vanguard + warden records present", vg != nullptr && wd != nullptr);
        if (vg == nullptr || wd == nullptr) { drop_tables(conn); return 1; }

        check("vanguard usable_armor {mail, plate} (from DB)",
              vg->usable_armor_types.size() == 2 && vg->can_use_armor(122) && vg->can_use_armor(120));
        check("vanguard usable_weapon {one_hand, two_hand} (from DB)",
              vg->usable_weapon_types.size() == 2 && vg->can_use_weapon(121) && vg->can_use_weapon(124));
        check("warden usable_armor {leather, mail} — NO plate (from DB)",
              wd->can_use_armor(119) && wd->can_use_armor(120) && !wd->can_use_armor(122));

        const itm::ItemTemplate* chest = items.find(kWardenChest);
        const itm::ItemTemplate* sword = items.find(kIronSword);
        const itm::ItemTemplate* cursed = items.find(kCursedBlade);
        check("item templates loaded (chest/sword/cursed)",
              chest != nullptr && sword != nullptr && cursed != nullptr);
        if (chest == nullptr || sword == nullptr || cursed == nullptr) { drop_tables(conn); return 1; }
        check("plate chest carries equip_type_id=plate (from DB)", chest->equip_type_id == 122);
        check("iron sword carries equip_type_id=one_hand (from DB)", sword->equip_type_id == 121);

        // ---- EQUIP GATE: ACCEPT ------------------------------------------------
        check("ACCEPT: vanguard equips the plate chest",
              gate_equip(*vg, equip_types, *chest) == EquipGate::kAllowed);
        check("ACCEPT: vanguard equips the iron sword (one_hand)",
              gate_equip(*vg, equip_types, *sword) == EquipGate::kAllowed);

        // ---- EQUIP GATE: REJECT (class cannot use the type) --------------------
        check("REJECT: warden CANNOT equip the plate chest (not proficient)",
              gate_equip(*wd, equip_types, *chest) == EquipGate::kNotProficient);

        // ---- EQUIP GATE: REJECT (category mismatch) ----------------------------
        // A one_hand (weapon-category) item slotted in a CHEST (armor) slot.
        check("REJECT: weapon-category item in an armor slot (category mismatch)",
              gate_equip(*vg, equip_types, *cursed) == EquipGate::kCategoryMismatch);
        check("REJECT: category mismatch holds for the warden too",
              gate_equip(*wd, equip_types, *cursed) == EquipGate::kCategoryMismatch);

        // ---- ROLE HOOK ---------------------------------------------------------
        check("ROLE: tank Vanguard amplifies threat (2.0x, from DB roles)",
              threat_multiplier(*vg) == kTankThreatMultiplier);
        check("ROLE: non-tank Warden threat is neutral (1.0x)",
              threat_multiplier(*wd) == 1.0f);

        // ---- TALENT APPLICATION (through the SP2.4 effective-stat framework) ----
        const TalentTreeDef* tree = talents.find_tree(vg->talent_tree_id);
        check("vanguard talent tree loaded (id 143 from DB)", tree != nullptr);
        if (tree != nullptr) {
            // A level-10 Vanguard with 5 talent points spent: both tiers unlock.
            const TalentApplication app = apply_talents(*tree, talents, /*points=*/5);
            check("TALENT: grants abilities 133 + 135 (from DB)",
                  app.granted_ability_ids.size() == 2 &&
                      app.granted_ability_ids[0] == 133 && app.granted_ability_ids[1] == 135);

            // The passive strength +5 folds into the SP2.4 effective-stat framework.
            // Base 10 str + class_mod +2 (from DB) = 12 at rest; + talent passive +5 = 17.
            EffectiveStats es(attrs, /*race=*/0, /*class=*/1);
            es.set_base("core:attribute.strength", 10);
            check("TALENT: strength at rest (base 10 + class +2) = 12",
                  es.static_value("core:attribute.strength") == 12);
            const AttributeDelta str_delta = app.passive("core:attribute.strength");
            check("TALENT: effective strength with talent passive = 12 + 5 = 17",
                  es.effective("core:attribute.strength", str_delta) == 17);
        }

        drop_tables(conn);
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all equip-gating + role/talent DB-backed checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d equip-gating DB-backed check(s) failed\n", g_fail);
    return 1;
}
