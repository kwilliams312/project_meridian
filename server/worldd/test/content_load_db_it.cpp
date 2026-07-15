// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB CONTENT LOAD DB-backed integration test (issue #390, epic #20).
//
// The acceptance bar for the content-load story: seed a world DB with the AUTHORED
// fixture content (server/worldd/test/fixtures/world_content — a 2-quest chain, the
// quest-giver + a loot-carrying creature, the items they reward/drop, a loot table,
// and a vendor), construct the DB-backed content stores against it, and assert each
// store returns the authored definitions behind the SAME seam the placeholder store
// implements:
//   * DbQuestStore     — a quest's objectives + rewards (granted + choice) + prereqs;
//   * DbNpcStore       — an NPC's roles (vendor flag, trainer role + taught abilities
//                        with class/level/cost gates) + quest giver/turn-in refs;
//   * DbLootTableStore — a creature's loot table entries, keyed by creature id, with
//                        the quest-gated drop + the creature's additive money;
//   * DbVendorCatalog  — a vendor's catalog + resolved prices (override + template);
//   * DbTemplateStore  — the item templates the above reference.
//
// The seeded rows MIRROR the `mcc emit-sql` output of the fixture pack
// (world_content.sql, committed alongside the YAML) — the same IF-9 numeric ids mcc
// assigned (items 1-4, loot_table 5, npc gray_wolf 6 / marshal_bren 7, quests 8-9,
// vendor 10). The fixture YAML is the authored source; running it through mcc emit-sql
// produced exactly these rows (verified in dev; see the PR). This test seeds them
// directly into minimal no-FK tables — the established DB-test pattern in this repo
// (world_boot_db_test, vendor_store_it, quest_loot_npc_db_it) — so it is fully
// deterministic and self-contained against any MariaDB, and does not depend on the
// full world DDL / FK graph being present.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (its own vars — the world DB is a separate DB
// per SAD §2.2) and falls back to MERIDIAN_DB_* so the existing DB CI job / test.sh
// --db (which set MERIDIAN_DB_*) run it unchanged. SKIPS (exit 0) when neither is set,
// so it is inert in the plain server ctest and runs for real only with a live MariaDB.
// It creates + drops the tables it owns, so it is idempotent against a DB that also
// carries other schemas (it only touches these content tables).
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + seams;
// no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "characters.h"  // create/list a character validated against the loaded roster (#695)
#include "db_content_store.h"
#include "meridian/db/connection.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace meridian;
namespace db = meridian::db;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

// Bind helpers for the seed inserts (values are fixture constants — parameterized to
// avoid any quoting/escaping, mirroring the backend "bind, never concatenate" rule).
db::Param i(std::int64_t v) { return db::Param{v}; }
db::Param s(const std::string& v) { return db::Param{v}; }
db::Param null() { return db::Param{std::monostate{}}; }

// The content tables the DB stores read, created WITHOUT foreign keys (the stores only
// SELECT columns; FK integrity is mcc's / the real DDL's concern). Columns match
// schema/sql/world/*.sql for the fields each store loads. DROP-then-CREATE so a rerun
// is clean.
void create_tables(db::Connection& c) {
    const char* drops[] = {
        "DROP TABLE IF EXISTS item_stat",
        "DROP TABLE IF EXISTS item_template",
        "DROP TABLE IF EXISTS quest_objective",
        "DROP TABLE IF EXISTS quest_prereq",
        "DROP TABLE IF EXISTS quest_reward",
        "DROP TABLE IF EXISTS quest_template",
        "DROP TABLE IF EXISTS loot_entry",
        "DROP TABLE IF EXISTS loot_group",
        "DROP TABLE IF EXISTS loot_table",
        "DROP TABLE IF EXISTS vendor_inventory_item",
        "DROP TABLE IF EXISTS vendor_inventory",
        "DROP TABLE IF EXISTS npc_trainer_ability",
        "DROP TABLE IF EXISTS npc_trainer",
        "DROP TABLE IF EXISTS npc_template",
        "DROP TABLE IF EXISTS area",
        "DROP TABLE IF EXISTS ability",
        "DROP TABLE IF EXISTS spawn_point",
    };
    for (const char* d : drops) c.execute(d);

    c.execute(
        "CREATE TABLE item_template ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  item_class VARCHAR(16) NOT NULL, slot VARCHAR(16) NULL,"
        "  equip_type_id INT UNSIGNED NULL,"  // SP2.7 #697 — DbTemplateStore SELECTs it
        "  rarity VARCHAR(16) NOT NULL, required_level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  item_level SMALLINT UNSIGNED NOT NULL DEFAULT 1, is_unique BOOLEAN NOT NULL DEFAULT FALSE,"
        "  binding VARCHAR(16) NOT NULL DEFAULT 'none', stack_size SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  weapon_damage_min INT UNSIGNED NULL, weapon_damage_max INT UNSIGNED NULL,"
        "  weapon_speed_ms INT UNSIGNED NULL, armor INT UNSIGNED NULL,"
        "  price_sell BIGINT UNSIGNED NULL, price_buy BIGINT UNSIGNED NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE item_stat ("
        "  item_id INT UNSIGNED NOT NULL, stat VARCHAR(16) NOT NULL, amount INT NOT NULL,"
        "  PRIMARY KEY (item_id, stat)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE quest_template ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  level SMALLINT UNSIGNED NOT NULL, required_level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  giver_npc_id INT UNSIGNED NOT NULL, turn_in_npc_id INT UNSIGNED NULL,"
        "  reward_xp INT UNSIGNED NULL, reward_money BIGINT UNSIGNED NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE quest_objective ("
        "  quest_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  type VARCHAR(16) NOT NULL, target_npc_id INT UNSIGNED NULL, item_id INT UNSIGNED NULL,"
        "  to_npc_id INT UNSIGNED NULL, zone_ref_id INT UNSIGNED NULL, poi VARCHAR(64) NULL,"
        "  count SMALLINT UNSIGNED NULL, PRIMARY KEY (quest_id, ordinal)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE quest_prereq ("
        "  quest_id INT UNSIGNED NOT NULL, prereq_quest_id INT UNSIGNED NOT NULL,"
        "  PRIMARY KEY (quest_id, prereq_quest_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE quest_reward ("
        "  quest_id INT UNSIGNED NOT NULL, is_choice BOOLEAN NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  item_id INT UNSIGNED NOT NULL, count SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  PRIMARY KEY (quest_id, is_choice, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE loot_table ("
        "  id INT UNSIGNED NOT NULL, money_min BIGINT UNSIGNED NULL, money_max BIGINT UNSIGNED NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE loot_group ("
        "  loot_table_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  name VARCHAR(64) NOT NULL, pick TINYINT UNSIGNED NOT NULL, chance_pct FLOAT NULL,"
        "  PRIMARY KEY (loot_table_id, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE loot_entry ("
        "  loot_table_id INT UNSIGNED NOT NULL, entry_ordinal SMALLINT UNSIGNED NOT NULL,"
        "  group_ordinal SMALLINT UNSIGNED NULL, item_id INT UNSIGNED NULL, nested_table_id INT UNSIGNED NULL,"
        "  chance_pct FLOAT NULL, weight INT UNSIGNED NULL, quantity_min INT UNSIGNED NULL,"
        "  quantity_max INT UNSIGNED NULL, quest_ref_id INT UNSIGNED NULL,"
        "  PRIMARY KEY (loot_table_id, entry_ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE vendor_inventory (id INT UNSIGNED NOT NULL, PRIMARY KEY (id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE vendor_inventory_item ("
        "  vendor_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  item_id INT UNSIGNED NOT NULL, price_override BIGINT UNSIGNED NULL,"
        "  limited_count INT UNSIGNED NULL, limited_restock_minutes INT UNSIGNED NULL,"
        "  PRIMARY KEY (vendor_id, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // npc_template — only the columns the NPC / loot stores read.
    c.execute(
        "CREATE TABLE npc_template ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL, vendor_ref_id INT UNSIGNED NULL,"
        "  loot_table_ref_id INT UNSIGNED NULL, loot_money_min BIGINT UNSIGNED NULL,"
        "  loot_money_max BIGINT UNSIGNED NULL, PRIMARY KEY (id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // area — POI rows the #398 area-trigger volume loader reads (pos + radius FLOATs).
    c.execute(
        "CREATE TABLE area ("
        "  zone_id INT UNSIGNED NOT NULL, poi VARCHAR(64) NOT NULL, name VARCHAR(80) NOT NULL,"
        "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
        "  discovery_radius_m FLOAT NOT NULL DEFAULT 40, discovery_xp INT UNSIGNED NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (zone_id, poi)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // npc_trainer / npc_trainer_ability — the #392 trainer role tables the NPC store
    // reads. Columns mirror schema/sql/world/10_npc.sql (required_class is the class-
    // name ENUM, NULL = any class). No FKs (the store only SELECTs, like the others).
    c.execute(
        "CREATE TABLE npc_trainer (npc_id INT UNSIGNED NOT NULL, PRIMARY KEY (npc_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE npc_trainer_ability ("
        "  npc_id INT UNSIGNED NOT NULL, ability_id INT UNSIGNED NOT NULL,"
        "  cost_copper BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "  required_class ENUM('vanguard','runcaller','warden','mender') NULL,"
        "  required_level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  PRIMARY KEY (npc_id, ability_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // ability — the #481 ability catalog load_world_content now also reads
    // (load_db_ability_store). This test does not assert on abilities, so the table
    // is created EMPTY — the loader returns an empty AbilityStore. Columns mirror
    // schema/sql/world/30_ability.sql (SP2.1 #691: effects[] rides in effects_json;
    // the per-kind child tables are retired). Without the table load_world_content
    // throws "Table 'ability' doesn't exist".
    c.execute(
        "CREATE TABLE ability ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  school ENUM('physical','fire','frost','nature','shadow','holy','arcane') NOT NULL,"
        "  target ENUM('self','enemy','friendly') NOT NULL, range_m FLOAT NOT NULL DEFAULT 5,"
        "  cast_time_ms INT UNSIGNED NULL DEFAULT 0, cast_channel_ms INT UNSIGNED NULL,"
        "  cooldown_ms INT UNSIGNED NOT NULL DEFAULT 0, triggers_gcd BOOLEAN NOT NULL DEFAULT TRUE,"
        "  resource_type ENUM('mana','rage','energy') NULL, resource_amount INT UNSIGNED NULL,"
        "  effects_json JSON NOT NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // spawn_point — the #486 spawn loader reads this (PASS 1). Created EMPTY here (this
    // test seeds no spawns), so load_spawn_points returns zero placements and never
    // touches the extended npc_template columns (level_min/faction/stat_health) this
    // minimal npc_template lacks. Columns mirror schema/sql/world/70_spawn.sql.
    c.execute(
        "CREATE TABLE spawn_point ("
        "  id INT UNSIGNED NOT NULL, zone_ref_id INT UNSIGNED NOT NULL DEFAULT 0,"
        "  npc_id INT UNSIGNED NOT NULL, pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL,"
        "  pos_z FLOAT NOT NULL, orientation_deg FLOAT NOT NULL DEFAULT 0,"
        "  respawn_min INT UNSIGNED NOT NULL DEFAULT 0, respawn_max INT UNSIGNED NOT NULL DEFAULT 0,"
        "  wander_radius_m FLOAT NULL, PRIMARY KEY (id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // race / class — the SP2.5 #695 roster loader load_world_content now also reads
    // (load_db_roster). Seeded below with the seed-pack roster; the loader merges
    // them with the compiled fallback. Columns mirror schema/sql/world/35_roster.sql.
    // Without the tables load_world_content throws "Table 'race' doesn't exist".
    c.execute(
        "CREATE TABLE race ("
        "  roster_id TINYINT UNSIGNED NOT NULL, content_id INT UNSIGNED NOT NULL,"
        "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
        "  PRIMARY KEY (roster_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE class ("
        "  roster_id TINYINT UNSIGNED NOT NULL, content_id INT UNSIGNED NOT NULL,"
        "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
        "  talent_tree_id INT UNSIGNED NULL,"  // SP2.7 #697 — load_db_class_catalog SELECTs it
        "  PRIMARY KEY (roster_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // class_race_limit — the SP2.6 #696 class race_limits gate load_db_roster now
    // also reads. Columns mirror schema/sql/world/35_roster.sql. Seeded below with
    // the seed pack's Vanguard→{Ardent,Dolmen} gate (Warden omits race_limits → no
    // rows → all races). Without the table load_db_roster throws "Table 'class_race_limit' doesn't exist".
    c.execute(
        "CREATE TABLE class_race_limit ("
        "  class_roster_id TINYINT UNSIGNED NOT NULL, race_roster_id TINYINT UNSIGNED NOT NULL,"
        "  PRIMARY KEY (class_roster_id, race_roster_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // attribute framework — the SP2.4 #694 loader load_world_content now also reads
    // (load_db_attributes). Without these tables it throws "Table 'attribute' doesn't
    // exist". Columns mirror schema/sql/world/36_attribute.sql (no FKs — the loader
    // only SELECTs, like the other self-seeded content tables here).
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
    // equip-gating + role/talent catalogs — the SP2.7 #697 loaders load_world_content
    // now also reads (load_db_equip_types / load_db_class_catalog child passes /
    // load_db_talents). Left empty here (this test asserts on quests/loot/vendors/etc.,
    // not the class kernel); without the tables load_world_content throws "Table
    // 'equip_type' doesn't exist". Columns mirror schema/sql/world/{15_equip_type,
    // 35_roster,38_talent}.sql. The dedicated proof is equip_gating_db_it.cpp.
    c.execute(
        "CREATE TABLE equip_type ("
        "  content_id INT UNSIGNED NOT NULL, equip_ref VARCHAR(96) NOT NULL,"
        "  name VARCHAR(64) NOT NULL, category ENUM('armor','weapon') NOT NULL,"
        "  slot_class VARCHAR(32) NULL, PRIMARY KEY (content_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE class_usable_equip_type ("
        "  class_roster_id TINYINT UNSIGNED NOT NULL, equip_type_id INT UNSIGNED NOT NULL,"
        "  list ENUM('armor','weapon') NOT NULL, PRIMARY KEY (class_roster_id, equip_type_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE class_role ("
        "  class_roster_id TINYINT UNSIGNED NOT NULL,"
        "  role ENUM('healer','dps_melee','dps_ranged','tank') NOT NULL,"
        "  PRIMARY KEY (class_roster_id, role))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE talent ("
        "  content_id INT UNSIGNED NOT NULL, talent_ref VARCHAR(96) NOT NULL,"
        "  name VARCHAR(64) NOT NULL, rank_max SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  PRIMARY KEY (content_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE talent_grant ("
        "  talent_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  kind ENUM('ability','buff','debuff') NOT NULL, ability_id INT UNSIGNED NULL,"
        "  attribute_ref VARCHAR(64) NULL, amount INT NULL, modifier ENUM('flat','percent') NULL,"
        "  duration_ms INT UNSIGNED NULL, max_stacks SMALLINT UNSIGNED NULL,"
        "  PRIMARY KEY (talent_id, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE talent_tree ("
        "  content_id INT UNSIGNED NOT NULL, tree_ref VARCHAR(96) NOT NULL,"
        "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
        "  PRIMARY KEY (content_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE talent_tree_tier ("
        "  talent_tree_id INT UNSIGNED NOT NULL, tier_ordinal SMALLINT UNSIGNED NOT NULL,"
        "  required_points SMALLINT UNSIGNED NOT NULL, PRIMARY KEY (talent_tree_id, tier_ordinal))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE talent_tree_tier_talent ("
        "  talent_tree_id INT UNSIGNED NOT NULL, tier_ordinal SMALLINT UNSIGNED NOT NULL,"
        "  ordinal SMALLINT UNSIGNED NOT NULL, talent_id INT UNSIGNED NOT NULL,"
        "  PRIMARY KEY (talent_tree_id, tier_ordinal, ordinal))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void drop_tables(db::Connection& c) {
    const char* drops[] = {
        "DROP TABLE IF EXISTS item_stat",       "DROP TABLE IF EXISTS item_template",
        "DROP TABLE IF EXISTS quest_objective", "DROP TABLE IF EXISTS quest_prereq",
        "DROP TABLE IF EXISTS quest_reward",    "DROP TABLE IF EXISTS quest_template",
        "DROP TABLE IF EXISTS loot_entry",      "DROP TABLE IF EXISTS loot_group",
        "DROP TABLE IF EXISTS loot_table",      "DROP TABLE IF EXISTS vendor_inventory_item",
        "DROP TABLE IF EXISTS vendor_inventory", "DROP TABLE IF EXISTS npc_trainer_ability",
        "DROP TABLE IF EXISTS npc_trainer",      "DROP TABLE IF EXISTS npc_template",
        "DROP TABLE IF EXISTS area",             "DROP TABLE IF EXISTS ability",
        "DROP TABLE IF EXISTS spawn_point",
        "DROP TABLE IF EXISTS class_race_limit",
        "DROP TABLE IF EXISTS class_attribute_mod", "DROP TABLE IF EXISTS race_attribute_mod",
        "DROP TABLE IF EXISTS attribute",
        "DROP TABLE IF EXISTS equip_type", "DROP TABLE IF EXISTS class_usable_equip_type",
        "DROP TABLE IF EXISTS class_role", "DROP TABLE IF EXISTS talent",
        "DROP TABLE IF EXISTS talent_grant", "DROP TABLE IF EXISTS talent_tree",
        "DROP TABLE IF EXISTS talent_tree_tier", "DROP TABLE IF EXISTS talent_tree_tier_talent",
        "DROP TABLE IF EXISTS race",             "DROP TABLE IF EXISTS class",
    };
    for (const char* d : drops) c.execute(d);
}

// Seed the fixture rows — the exact IF-9 ids + values `mcc emit-sql` produced for
// server/worldd/test/fixtures/world_content (world_content.sql).
void seed_fixture(db::Connection& c) {
    // Items 1-4.
    auto item = [&](int id, const char* name, const char* cls, db::Param slot, const char* rarity,
                    int req_lvl, int stack, db::Param armor, db::Param sell, db::Param buy) {
        c.execute(
            "INSERT INTO item_template (id, name, item_class, slot, rarity, required_level, "
            "item_level, is_unique, binding, stack_size, armor, price_sell, price_buy) "
            "VALUES (?, ?, ?, ?, ?, ?, 1, FALSE, 'none', ?, ?, ?, ?)",
            {i(id), s(name), s(cls), slot, s(rarity), i(req_lvl), i(stack), armor, sell, buy});
    };
    item(1, "Draught of Mending", "consumable", null(), "common", 1, 5, null(), i(30), i(150));
    item(2, "Ironbound Trinket", "armor", s("trinket"), "uncommon", 3, 1, null(), i(200), i(800));
    item(3, "Traveler's Boots", "armor", s("feet"), "common", 2, 1, i(14), i(100), i(400));
    item(4, "Coarse Wolf Pelt", "trade_good", null(), "common", 1, 20, null(), i(12), null());
    c.execute("INSERT INTO item_stat (item_id, stat, amount) VALUES (2, 'stamina', 4)");

    // npc_template: gray_wolf (6) loot creature; marshal_bren (7) vendor 10.
    c.execute(
        "INSERT INTO npc_template (id, name, vendor_ref_id, loot_table_ref_id, loot_money_min, "
        "loot_money_max) VALUES (6, 'Gray Wolf', NULL, 5, 2, 6)");
    c.execute(
        "INSERT INTO npc_template (id, name, vendor_ref_id, loot_table_ref_id, loot_money_min, "
        "loot_money_max) VALUES (7, ?, 10, NULL, NULL, NULL)",
        {s("Marshal Bren")});

    // Trainer role (#392): Marshal Bren (7) also teaches two abilities. These
    // npc_trainer / npc_trainer_ability rows exercise the DbNpcStore trainer loader
    // and follow the EXACT shape mcc emit-sql produces (see content/core's warden_sela
    // in tools/mcc/golden/world.sql). The fixture pack predates the abilities module,
    // so the taught ability ids (101/102) are representative — the loader does not read
    // an ability table. ability 101: any class (NULL), level 2, 50c; ability 102:
    // Vanguard-only (roster class 1), level 5, 120c.
    c.execute("INSERT INTO npc_trainer (npc_id) VALUES (7)");
    c.execute(
        "INSERT INTO npc_trainer_ability (npc_id, ability_id, cost_copper, required_class, "
        "required_level) VALUES (7, 101, 50, NULL, 2)");
    c.execute(
        "INSERT INTO npc_trainer_ability (npc_id, ability_id, cost_copper, required_class, "
        "required_level) VALUES (7, 102, 120, 'vanguard', 5)");

    // Quests 8 (pelts, prereq 9) + 9 (thin the pack).
    c.execute(
        "INSERT INTO quest_template (id, name, level, required_level, giver_npc_id, turn_in_npc_id, "
        "reward_xp, reward_money) VALUES (8, ?, 7, 4, 7, NULL, 320, 150)",
        {s("Pelts for Bren")});
    c.execute(
        "INSERT INTO quest_template (id, name, level, required_level, giver_npc_id, turn_in_npc_id, "
        "reward_xp, reward_money) VALUES (9, ?, 6, 4, 7, NULL, 250, 300)",
        {s("Thin the Pack")});
    c.execute(
        "INSERT INTO quest_objective (quest_id, ordinal, type, target_npc_id, item_id, to_npc_id, "
        "zone_ref_id, poi, count) VALUES (8, 0, 'collect', NULL, 4, NULL, NULL, NULL, 3)");
    c.execute(
        "INSERT INTO quest_objective (quest_id, ordinal, type, target_npc_id, item_id, to_npc_id, "
        "zone_ref_id, poi, count) VALUES (9, 0, 'kill', 6, NULL, NULL, NULL, NULL, 5)");
    c.execute("INSERT INTO quest_prereq (quest_id, prereq_quest_id) VALUES (8, 9)");
    c.execute("INSERT INTO quest_reward (quest_id, is_choice, ordinal, item_id, count) VALUES (8, TRUE, 0, 2, 1)");
    c.execute("INSERT INTO quest_reward (quest_id, is_choice, ordinal, item_id, count) VALUES (8, TRUE, 1, 3, 1)");
    c.execute("INSERT INTO quest_reward (quest_id, is_choice, ordinal, item_id, count) VALUES (9, FALSE, 0, 1, 2)");

    // Loot table 5: money 4-10; group 'scraps' (chance 35) w/ item1 w10 + item2 w1; a
    // top-level quest-gated wolf_pelt (item4) drop at chance 60 (quest 8).
    c.execute("INSERT INTO loot_table (id, money_min, money_max) VALUES (5, 4, 10)");
    c.execute("INSERT INTO loot_group (loot_table_id, ordinal, name, pick, chance_pct) VALUES (5, 0, 'scraps', 1, 35)");
    c.execute(
        "INSERT INTO loot_entry (loot_table_id, entry_ordinal, group_ordinal, item_id, nested_table_id, "
        "chance_pct, weight, quantity_min, quantity_max, quest_ref_id) "
        "VALUES (5, 0, NULL, 4, NULL, 60, NULL, 1, 1, 8)");
    c.execute(
        "INSERT INTO loot_entry (loot_table_id, entry_ordinal, group_ordinal, item_id, nested_table_id, "
        "chance_pct, weight, quantity_min, quantity_max, quest_ref_id) "
        "VALUES (5, 1, 0, 1, NULL, NULL, 10, NULL, NULL, NULL)");
    c.execute(
        "INSERT INTO loot_entry (loot_table_id, entry_ordinal, group_ordinal, item_id, nested_table_id, "
        "chance_pct, weight, quantity_min, quantity_max, quest_ref_id) "
        "VALUES (5, 2, 0, 2, NULL, NULL, 1, NULL, NULL, NULL)");

    // Two POI rows (zone 20): 'ridge_overlook' (radius 40) + 'old_well' (radius 25).
    // The #398 loader turns each into a discovery TriggerVolume carrying (zone_id, poi).
    c.execute(
        "INSERT INTO area (zone_id, poi, name, pos_x, pos_y, pos_z, discovery_radius_m, "
        "discovery_xp) VALUES (20, 'ridge_overlook', 'Ridge Overlook', 100, 200, 5, 40, 120)");
    c.execute(
        "INSERT INTO area (zone_id, poi, name, pos_x, pos_y, pos_z, discovery_radius_m, "
        "discovery_xp) VALUES (20, 'old_well', 'The Old Well', -50, -80, 0, 25, 60)");

    // Vendor 10: item1 (no override -> template buy 150), item3 (override 500).
    c.execute("INSERT INTO vendor_inventory (id) VALUES (10)");
    c.execute(
        "INSERT INTO vendor_inventory_item (vendor_id, ordinal, item_id, price_override, limited_count, "
        "limited_restock_minutes) VALUES (10, 0, 1, NULL, NULL, NULL)");
    c.execute(
        "INSERT INTO vendor_inventory_item (vendor_id, ordinal, item_id, price_override, limited_count, "
        "limited_restock_minutes) VALUES (10, 1, 3, 500, NULL, NULL)");

    // race / class roster (SP2.5 #695) — the seed-pack rows keyed by roster_id (the
    // canonical character.race/class id), content_id = the IF-9 numeric id mcc emits.
    // Ardent=1/Dolmen=2 races, Vanguard=1/Warden=3 classes (Warden=3 skips the not-
    // yet-in-pack Runcaller=2). load_db_roster merges these with the compiled fallback
    // (Sylvane=3/Emberkin=4 races, Runcaller=2/Mender=4 classes).
    c.execute("INSERT INTO race (roster_id, content_id, name, description) VALUES "
              "(1, 139, 'Ardent', 'Resilient folk of the central realm.'),"
              "(2, 140, 'Dolmen', 'Stoic mountain folk hewn from the deep stone.')");
    c.execute("INSERT INTO class (roster_id, content_id, name, description) VALUES "
              "(1, 137, 'Vanguard', 'A front-line melee defender who holds the line.'),"
              "(3, 138, 'Warden', 'A ranged hybrid who mends allies and burns foes at distance.')");
    // class_race_limit (SP2.6 #696) — the seed pack's Vanguard→{Ardent=1,Dolmen=2}
    // race_limits gate. Warden omits race_limits (no rows → all races permitted).
    // Mirrors what mcc emit-sql produces from content/core/classes/vanguard.class.yaml.
    c.execute("INSERT INTO class_race_limit (class_roster_id, race_roster_id) VALUES "
              "(1, 1), (1, 2)");
    // attribute framework (SP2.4 #694) — the seed-pack vocabulary (a primary + a
    // derived) and the Vanguard's class attribute_mods (strength +2, stamina +1).
    // load_db_attributes reads these into WorldContent.attributes.
    c.execute("INSERT INTO attribute (attr_ref, content_id, name, kind) VALUES "
              "('core:attribute.strength', 132, 'Strength', 'primary'),"
              "('core:attribute.stamina', 131, 'Stamina', 'primary'),"
              "('core:attribute.armor', 127, 'Armor', 'derived')");
    c.execute("INSERT INTO class_attribute_mod (class_roster_id, attr_ref, value) VALUES "
              "(1, 'core:attribute.strength', 2),"
              "(1, 'core:attribute.stamina', 1)");
}

// Find an objective of a given type in a quest def.
const worldd::QuestObjective* obj_of_type(const worldd::QuestDef& q, worldd::ObjectiveType t) {
    for (const auto& o : q.objectives)
        if (o.type == t) return &o;
    return nullptr;
}

}  // namespace

int main() {
    std::printf("worldd world-DB content-load DB-backed integration test (#390)\n");

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
                    "content-load checks skipped (set MERIDIAN_WORLDDB_HOST + "
                    "MERIDIAN_WORLDDB_USER, or reuse MERIDIAN_DB_*)\n");
        return 0;
    }

    try {
        db::Connection conn(p);
        create_tables(conn);
        seed_fixture(conn);

        // Load every store from the seeded world DB (the boot load path).
        worldd::WorldContent content = worldd::load_world_content(conn);

        // ---- DbTemplateStore ---------------------------------------------------
        {
            const items::ItemTemplate* draught = content.items->find(1);
            check("item 1 (draught) loaded", draught != nullptr);
            if (draught) {
                check("item 1 name", draught->name == "Draught of Mending");
                check("item 1 class consumable", draught->item_class == items::ItemClass::kConsumable);
                check("item 1 stack 5", draught->max_stack == 5);
                check("item 1 buy_price 150", draught->buy_price && *draught->buy_price == 150);
                check("item 1 sell_price 30", draught->sell_price && *draught->sell_price == 30);
            }
            const items::ItemTemplate* trinket = content.items->find(2);
            check("item 2 (trinket) has stamina stat", trinket && trinket->stats.size() == 1 &&
                                                          trinket->stats[0].stat == items::StatKey::kStamina &&
                                                          trinket->stats[0].amount == 4);
            const items::ItemTemplate* boots = content.items->find(3);
            check("item 3 (boots) armor 14 + slot feet",
                  boots && boots->armor == 14 && boots->slot == items::ItemSlot::kFeet);
            const items::ItemTemplate* pelt = content.items->find(4);
            check("item 4 (pelt) no buy price (not sold)", pelt && !pelt->buy_price.has_value());
        }

        // ---- DbQuestStore ------------------------------------------------------
        {
            const worldd::QuestDef* thin = content.quests->find(9);
            check("quest 9 (thin the pack) loaded", thin != nullptr);
            if (thin) {
                check("quest 9 name", thin->name == "Thin the Pack");
                check("quest 9 giver npc 7", thin->giver_npc_id == 7);
                check("quest 9 turn_in defaults to giver", thin->turn_in_npc() == 7);
                check("quest 9 reward xp 250", thin->reward_xp == 250);
                check("quest 9 reward money 300", thin->reward_money == 300);
                const worldd::QuestObjective* kill = obj_of_type(*thin, worldd::ObjectiveType::kKill);
                check("quest 9 kill objective npc 6 x5",
                      kill && kill->target_npc_id == 6 && kill->count == 5);
                check("quest 9 no prerequisites", thin->prerequisites.empty());
                check("quest 9 grants 1 always-item (draught x2)",
                      thin->reward_items.size() == 1 && thin->reward_items[0].item_id == 1 &&
                          thin->reward_items[0].count == 2);
                check("quest 9 has no choice items", thin->choice_items.empty());
            }

            const worldd::QuestDef* pelts = content.quests->find(8);
            check("quest 8 (pelts) loaded", pelts != nullptr);
            if (pelts) {
                check("quest 8 prereq is quest 9",
                      pelts->prerequisites.size() == 1 && pelts->prerequisites[0] == 9);
                const worldd::QuestObjective* col = obj_of_type(*pelts, worldd::ObjectiveType::kCollect);
                check("quest 8 collect objective item 4 x3",
                      col && col->item_id == 4 && col->count == 3);
                check("quest 8 two choice items (trinket 2 + boots 3)",
                      pelts->choice_items.size() == 2 && pelts->choice_items[0].item_id == 2 &&
                          pelts->choice_items[1].item_id == 3);
                check("quest 8 no always-granted items", pelts->reward_items.empty());
            }
        }

        // ---- DbNpcStore --------------------------------------------------------
        {
            const npc::NpcDef* bren = content.npcs->find(7);
            check("npc 7 (marshal bren) loaded", bren != nullptr);
            if (bren) {
                check("npc 7 name", bren->name == "Marshal Bren");
                check("npc 7 is a vendor (vendor_ref set)", bren->is_vendor);
                // Trainer role loaded from npc_trainer_ability (#392).
                check("npc 7 is a trainer (npc_trainer_ability rows)", bren->is_trainer);
                check("npc 7 teaches 2 abilities", bren->trainer_abilities.size() == 2);
                const npc::TrainerAbility* a101 = bren->trainer_ability(101);
                const npc::TrainerAbility* a102 = bren->trainer_ability(102);
                check("npc 7 teaches ability 101: any class (0), level 2, cost 50",
                      a101 && a101->required_class == 0 && a101->required_level == 2 &&
                          a101->cost == 50);
                check("npc 7 teaches ability 102: Vanguard (class 1), level 5, cost 120",
                      a102 && a102->required_class == 1 && a102->required_level == 5 &&
                          a102->cost == 120);
                // Bren gives + turns in BOTH quests (8, 9). turn_in defaults to giver,
                // so each quest ref is gives+turn_in.
                bool q8 = false, q9 = false, all_gt = true;
                for (const auto& ref : bren->quests) {
                    if (ref.quest_id == 8) q8 = true;
                    if (ref.quest_id == 9) q9 = true;
                    if (!(ref.gives && ref.turn_in)) all_gt = false;
                }
                check("npc 7 participates in quests 8 + 9", q8 && q9 && bren->quests.size() == 2);
                check("npc 7 gives + turns in each quest", all_gt);
            }
            const npc::NpcDef* wolf = content.npcs->find(6);
            check("npc 6 (gray wolf) loaded, not a vendor/trainer",
                  wolf && !wolf->is_vendor && !wolf->is_trainer &&
                      wolf->trainer_abilities.empty() && wolf->quests.empty());
        }

        // ---- DbLootTableStore (keyed by creature id) ---------------------------
        {
            const loot::LootTable* wolf_loot = content.loot->find(6);
            check("loot for creature 6 (gray wolf) loaded", wolf_loot != nullptr);
            if (wolf_loot) {
                // money = table(4-10) + creature(2-6) additive (D-25).
                check("loot money additive 6-16",
                      wolf_loot->money_min == 6 && wolf_loot->money_max == 16);
                // Two groups: the 'scraps' group (chance 3500 bp) + the top-level
                // quest-gated wolf_pelt drop as its own single-entry group (6000 bp).
                check("loot has 2 groups", wolf_loot->groups.size() == 2);
                // Locate the quest-gated single-entry group + the weighted scraps group.
                const loot::LootGroup* gated = nullptr;
                const loot::LootGroup* scraps = nullptr;
                for (const auto& g : wolf_loot->groups) {
                    if (g.entries.size() == 1 && g.entries[0].required_quest_id != 0) gated = &g;
                    else if (g.entries.size() == 2) scraps = &g;
                }
                check("quest-gated wolf_pelt drop: item 4, chance 6000 bp, quest 8",
                      gated && gated->chance_bp == 6000 && gated->entries[0].item_template_id == 4 &&
                          gated->entries[0].required_quest_id == 8);
                check("scraps group chance 3500 bp, item1 w10 + item2 w1",
                      scraps && scraps->chance_bp == 3500 &&
                          scraps->entries[0].item_template_id == 1 && scraps->entries[0].weight == 10 &&
                          scraps->entries[1].item_template_id == 2 && scraps->entries[1].weight == 1);
            }
            check("no loot for an unknown creature", content.loot->find(99999) == nullptr);
        }

        // ---- DbVendorCatalog ---------------------------------------------------
        {
            const std::vector<vendor::VendorListing>* cat = content.vendor->listings(10);
            check("vendor 10 catalog loaded", cat != nullptr);
            if (cat) {
                check("vendor 10 sells 2 items", cat->size() == 2);
                // Listing 0: item 1, no override -> price resolves to template buy_price 150.
                // Listing 1: item 3, override 500.
                check("vendor 10 listing 0 is item 1 (no override)",
                      cat->size() == 2 && (*cat)[0].item_template_id == 1 &&
                          !(*cat)[0].price_override.has_value());
                check("vendor 10 listing 1 is item 3 override 500",
                      cat->size() == 2 && (*cat)[1].item_template_id == 3 &&
                          (*cat)[1].price_override && *(*cat)[1].price_override == 500);
                if (cat->size() == 2) {
                    // Resolved BUY prices against the DB templates (the seam's price rule).
                    auto p0 = vendor::VendorCatalog::buy_price((*cat)[0], *content.items);
                    auto p1 = vendor::VendorCatalog::buy_price((*cat)[1], *content.items);
                    check("vendor price item1 resolves to template 150", p0 && *p0 == 150);
                    check("vendor price item3 resolves to override 500", p1 && *p1 == 500);
                }
            }
            check("unknown vendor -> nullptr", content.vendor->listings(99999) == nullptr);
        }

        // ---- area-trigger POI volumes (#398) -----------------------------------
        {
            check("two POI discovery volumes loaded from area rows",
                  content.area_triggers.size() == 2);
            // Locate each authored POI volume by its (zone_id, poi) join key.
            const worldd::TriggerVolume* ridge = nullptr;
            const worldd::TriggerVolume* well = nullptr;
            for (const auto& v : content.area_triggers) {
                if (v.area_id == 20 && v.poi == "ridge_overlook") ridge = &v;
                if (v.area_id == 20 && v.poi == "old_well") well = &v;
            }
            check("ridge_overlook volume: zone 20, discovery kind",
                  ridge && ridge->kind == worldd::TriggerKind::kDiscovery);
            check("old_well volume: zone 20, discovery kind",
                  well && well->kind == worldd::TriggerKind::kDiscovery);
            if (ridge) {
                // Box is the POI centre (100,200) inflated by radius 40 on (x,y).
                check("ridge_overlook box centred on pos +/- discovery_radius",
                      ridge->min_x == 60.0f && ridge->max_x == 140.0f &&
                          ridge->min_y == 160.0f && ridge->max_y == 240.0f);
                worldd::Position centre;
                centre.x = 100.0f; centre.y = 200.0f; centre.z = 0.0f;
                check("player at the POI centre is inside the volume", ridge->contains(centre));
            }
            if (well) {
                check("old_well box uses its own 25 m radius",
                      well->min_x == -75.0f && well->max_x == -25.0f);
            }
        }

        // ---- Roster (SP2.5 #695) — loaded from the `race`/`class` rows, MERGED with
        // the compiled fallback. Proves the retired roster.h enum is now sourced from
        // pack data at boot (character CREATE validates against this).
        {
            check("roster loaded from world DB", content.roster.has_value());
            if (content.roster) {
                const characters::Roster& r = *content.roster;
                // Pack entries (from the seeded race/class rows).
                check("roster: race Ardent(1) from pack",
                      r.is_valid_race(1) && r.race_name(1) == "Ardent");
                check("roster: race Dolmen(2) from pack",
                      r.is_valid_race(2) && r.race_name(2) == "Dolmen");
                check("roster: class Vanguard(1) from pack",
                      r.is_valid_class(1) && r.class_name(1) == "Vanguard");
                check("roster: class Warden(3) from pack",
                      r.is_valid_class(3) && r.class_name(3) == "Warden");
                // Compiled fallback entries (not yet authorable in the pack), merged.
                check("roster: race Sylvane(3) from fallback",
                      r.is_valid_race(3) && r.race_name(3) == "Sylvane");
                check("roster: race Emberkin(4) from fallback",
                      r.is_valid_race(4) && r.race_name(4) == "Emberkin");
                check("roster: class Runcaller(2) from fallback",
                      r.is_valid_class(2) && r.class_name(2) == "Runcaller");
                check("roster: class Mender(4) from fallback",
                      r.is_valid_class(4) && r.class_name(4) == "Mender");
                // Full M0 set = 4 races + 4 classes; 0 and out-of-range reject.
                check("roster: 4 races + 4 classes merged",
                      r.race_count() == 4 && r.class_count() == 4);
                check("roster: id 0 and 5 are invalid",
                      !r.is_valid_race(0) && !r.is_valid_race(5) &&
                      !r.is_valid_class(0) && !r.is_valid_class(5));

                // ---- R1 (chibi-theme design §4 / #754): the PACK ROSTER is
                // authoritative over the compiled roster.h fallback for any id it
                // ships. A pack race/class row that reuses a FALLBACK id (Sylvane=3,
                // Mender=4) must SUPERSEDE the fallback entry — this is exactly what
                // removes the char-create `catalog:3|0`/`4|0` errors for a theme pack
                // (e.g. chibi) that ships those ids WITH a catalog, instead of the
                // catalog-less compiled Sylvane/Emberkin leaking through. Seed a pack
                // row at each fallback id and re-load: the pack name wins, and the
                // roster size is unchanged (the id is REPLACED, not added).
                conn.execute(
                    "INSERT INTO race (roster_id, content_id, name, description) "
                    "VALUES (3, 200, 'ChibiForest', 'A themed race that ships a catalog.')");
                conn.execute(
                    "INSERT INTO class (roster_id, content_id, name, description) "
                    "VALUES (4, 201, 'ChibiMystic', 'A themed class shipped in-pack.')");
                const characters::Roster merged = worldd::load_db_roster(conn);
                check("R1 pack race supersedes same-id fallback (3: Sylvane -> ChibiForest)",
                      merged.is_valid_race(3) && merged.race_name(3) == "ChibiForest");
                check("R1 pack class supersedes same-id fallback (4: Mender -> ChibiMystic)",
                      merged.is_valid_class(4) && merged.class_name(4) == "ChibiMystic");
                check("R1 supersede REPLACES (not adds): still 4 races + 4 classes",
                      merged.race_count() == 4 && merged.class_count() == 4);
                // A fallback id the pack does NOT ship still remains the fallback —
                // roster.h stays the fallback ONLY for entries a pack omits.
                check("R1 unshipped fallback id remains (race 4 still Emberkin)",
                      merged.is_valid_race(4) && merged.race_name(4) == "Emberkin");

                // ---- Character CREATE + LOAD round-trips against the DB-LOADED
                // roster (the SP2.5 #695 acceptance bar): a character with a seed
                // race/class (Ardent=1 / Vanguard=1, both from the pack rows) is
                // created — validated against `r`, the roster loaded from MariaDB —
                // then read back with its ids intact; an id NOT in the roster is
                // refused. Uses a standalone `character` table on the same MariaDB
                // (no FK — the soft-ref account_id rule, §4.4).
                conn.execute(
                    "CREATE TEMPORARY TABLE `character` ("
                    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
                    "  account_id BIGINT UNSIGNED NOT NULL, name VARCHAR(32) NOT NULL,"
                    "  race TINYINT UNSIGNED NOT NULL, class TINYINT UNSIGNED NOT NULL,"
                    "  appearance JSON NULL, level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
                    "  map_id INT UNSIGNED NOT NULL DEFAULT 0,"
                    "  pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0,"
                    "  pos_z FLOAT NOT NULL DEFAULT 0, PRIMARY KEY (id),"
                    "  UNIQUE KEY uq_character_name (name)"
                    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
                characters::CreateRequest cr;
                cr.account_id = 424242;
                cr.name = "RosterProof";
                cr.race = 1;         // Ardent — a PACK roster id
                cr.char_class = 1;   // Vanguard — a PACK roster id
                bool created_ok = false;
                try {
                    characters::create_character(conn, cr, r);  // validate vs DB roster
                    created_ok = true;
                } catch (const std::exception&) {
                    created_ok = false;
                }
                check("create character (Ardent/Vanguard) vs DB-loaded roster", created_ok);

                std::vector<characters::CharacterSummary> mine =
                    characters::list_characters(conn, 424242);
                check("created character round-trips (race=1, class=1, name)",
                      mine.size() == 1 && mine[0].race == 1 &&
                          mine[0].char_class == 1 && mine[0].name == "RosterProof");

                // A race id NOT in the loaded roster is refused (InvalidRace).
                characters::CreateRequest bad = cr;
                bad.name = "BadRace";
                bad.race = 200;  // not in the roster
                bool refused = false;
                try {
                    characters::create_character(conn, bad, r);
                } catch (const characters::InvalidRace&) {
                    refused = true;
                }
                check("create with an unknown race id is refused (InvalidRace)", refused);

                // A class id NOT in the loaded roster is refused (InvalidClass).
                characters::CreateRequest bad_class = cr;
                bad_class.name = "BadClass";
                bad_class.char_class = 200;  // not in the roster
                bool refused_class = false;
                try {
                    characters::create_character(conn, bad_class, r);
                } catch (const characters::InvalidClass&) {
                    refused_class = true;
                }
                check("create with an unknown class id is refused (InvalidClass)",
                      refused_class);

                // ---- SP2.6 #696: race ∈ class race_limits gate, against the
                // DB-LOADED class_race_limit rows. Vanguard(1) is gated to
                // {Ardent=1, Dolmen=2}; Warden(3) omits race_limits (all races).
                // (a) A race NOT in the class's race_limits is refused with the
                //     DISTINCT InvalidRaceForClass — both ids valid, combo refused.
                //     Sylvane(3) is a VALID race (compiled fallback) but ∉ Vanguard's
                //     {1,2}, so it exercises the gate rather than InvalidRace.
                characters::CreateRequest gated = cr;
                gated.name = "SylvaneVanguard";
                gated.race = 3;        // Sylvane — valid race, NOT in Vanguard limits
                gated.char_class = 1;  // Vanguard — gated to {1,2}
                bool gate_refused = false;
                bool wrong_exc = false;
                try {
                    characters::create_character(conn, gated, r);
                } catch (const characters::InvalidRaceForClass&) {
                    gate_refused = true;
                } catch (const characters::InvalidRace&) {
                    wrong_exc = true;  // must NOT collapse to unknown-race
                } catch (const characters::InvalidClass&) {
                    wrong_exc = true;
                }
                check("create race∉class race_limits refused (InvalidRaceForClass)",
                      gate_refused);
                check("race_limits refusal is DISTINCT from InvalidRace/InvalidClass",
                      !wrong_exc);

                // (b) A race IN the class's race_limits is accepted. Dolmen(2) ∈
                //     Vanguard's {1,2}.
                characters::CreateRequest in_limits = cr;
                in_limits.name = "DolmenVanguard";
                in_limits.race = 2;        // Dolmen ∈ Vanguard limits {1,2}
                in_limits.char_class = 1;  // Vanguard
                bool in_ok = false;
                try {
                    characters::create_character(conn, in_limits, r);
                    in_ok = true;
                } catch (const std::exception&) {
                    in_ok = false;
                }
                check("create race∈class race_limits accepted (Dolmen/Vanguard)", in_ok);

                // (c) A class with NO race_limits (Warden=3) permits ANY valid race,
                //     including Sylvane(3) that Vanguard rejects — proves the
                //     "no rows = all races" semantics against the DB-loaded gate.
                characters::CreateRequest ungated = cr;
                ungated.name = "SylvaneWarden";
                ungated.race = 3;        // Sylvane
                ungated.char_class = 3;  // Warden — no race_limits rows → all races
                bool ungated_ok = false;
                try {
                    characters::create_character(conn, ungated, r);
                    ungated_ok = true;
                } catch (const std::exception&) {
                    ungated_ok = false;
                }
                check("create any race for a class without race_limits accepted "
                      "(Sylvane/Warden)", ungated_ok);
            }
        }

        // ---- Attribute framework (SP2.4 #694) — load_world_content reads the
        // `attribute` + `class_attribute_mod` tables into WorldContent.attributes.
        // Full effective-stat math is proven in worldd-effective-stats(-db); here we
        // just confirm the catalog is populated on the same load path.
        {
            const worldd::AttributeCatalog& a = content.attributes;
            check("attributes: 3 loaded (2 primary + 1 derived)",
                  a.attribute_count() == 3 && a.primary_count() == 2 && a.derived_count() == 1);
            check("attributes: strength is primary",
                  a.is_primary("core:attribute.strength"));
            check("attributes: armor is derived",
                  a.find("core:attribute.armor") != nullptr &&
                      !a.is_primary("core:attribute.armor"));
            check("attributes: Vanguard(1) strength mod +2",
                  a.class_mod(1, "core:attribute.strength") == 2);
            check("attributes: Vanguard(1) stamina mod +1",
                  a.class_mod(1, "core:attribute.stamina") == 1);
        }

        drop_tables(conn);
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all content-load checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d content-load check(s) failed\n", g_fail);
    return 1;
}
