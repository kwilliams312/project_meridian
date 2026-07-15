// SPDX-License-Identifier: Apache-2.0
//
// worldd — CONTENT SPAWN load + spawn-into-world DB-backed integration test
// (issue #486, epic #20). The acceptance bar for the spawn story: worldd previously
// loaded NPC TEMPLATES (DbNpcStore, #390) but never SPAWNED live creatures/NPCs — the
// seeded `spawn_point` content was unread, so a player entered and "saw 0 other(s)".
// This test proves the closed seam end to end:
//
//   1. LOAD — load_world_content reads spawn_point, resolving each row against its
//      npc_template (name + level + faction + health), into WorldContent.spawns.
//   2. INSTALL — WorldServer::install_spawns spawns each placement BOTH into the map
//      tick (existence: map_creature_count > 0) AND the AoI relay (a static world
//      entity: world_entity_count > 0) at the authored positions.
//   3. VISIBILITY — a session entering AoI near a spawn receives an ENTITY_ENTER for
//      it (carrying the #430 vitals + name), so the client renders it.
//   4. INTERACTABILITY — the spawned gossip NPC's wire guid resolves to its
//      npc_template id (npc_template_for_guid) so GOSSIP_HELLO serves its content;
//      the spawned entity is reachable by guid (unit_for_guid) for a cast/kill; and a
//      HOSTILE creature is present for a kill objective.
//
// Self-seeded no-FK tables (the established DB-test pattern — content_load_db_it,
// world_boot_db_test): the stores only SELECT, so FK integrity is mcc's concern. This
// keeps the test deterministic and self-contained against any MariaDB.
//
// DB-GATED: reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0)
// when neither is set, so it is inert in the plain server ctest and runs for real only
// with a live MariaDB (test.sh --db / the DB CI job). It creates + drops the tables it
// owns, so it is idempotent.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + seams; no
// GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "db_content_store.h"
#include "world_dispatch.h"
#include "world_state.h"
#include "world_generated.h"

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace meridian;
namespace db = meridian::db;
namespace mw = meridian::worldd;
namespace mn = meridian::net;
namespace fb = flatbuffers;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

// The content ids the fixture seeds.
constexpr std::uint32_t kGossipNpc = 100;   // friendly quest-giver NPC (GOSSIP_HELLO target)
constexpr std::uint32_t kDewdropNpc = 200;  // synthetic chibi:npc.dewdrop_slime
constexpr std::uint32_t kSproutcapNpc = 201; // synthetic chibi:npc.sproutcap_scamp
constexpr std::uint32_t kGiverQuest = 300;  // the quest kGossipNpc gives (so gossip is non-empty)

// The tables load_world_content SELECTs from. Created WITHOUT foreign keys (the stores
// only read). Only the columns each store loads are present; npc_template carries the
// EXTENDED combat columns the #486 spawn loader resolves (level_min/faction/stat_*).
void create_tables(db::Connection& c) {
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
        "DROP TABLE IF EXISTS graveyard",        "DROP TABLE IF EXISTS zone",
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
        "CREATE TABLE item_stat (item_id INT UNSIGNED NOT NULL, stat VARCHAR(16) NOT NULL,"
        "  amount INT NOT NULL, PRIMARY KEY (item_id, stat))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
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
        "CREATE TABLE quest_prereq (quest_id INT UNSIGNED NOT NULL, prereq_quest_id INT UNSIGNED NOT NULL,"
        "  PRIMARY KEY (quest_id, prereq_quest_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE quest_reward (quest_id INT UNSIGNED NOT NULL, is_choice BOOLEAN NOT NULL,"
        "  ordinal SMALLINT UNSIGNED NOT NULL, item_id INT UNSIGNED NOT NULL,"
        "  count SMALLINT UNSIGNED NOT NULL DEFAULT 1, PRIMARY KEY (quest_id, is_choice, ordinal))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE loot_table (id INT UNSIGNED NOT NULL, money_min BIGINT UNSIGNED NULL,"
        "  money_max BIGINT UNSIGNED NULL, PRIMARY KEY (id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE loot_group (loot_table_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  name VARCHAR(64) NOT NULL, pick TINYINT UNSIGNED NOT NULL, chance_pct FLOAT NULL,"
        "  PRIMARY KEY (loot_table_id, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE loot_entry (loot_table_id INT UNSIGNED NOT NULL, entry_ordinal SMALLINT UNSIGNED NOT NULL,"
        "  group_ordinal SMALLINT UNSIGNED NULL, item_id INT UNSIGNED NULL, nested_table_id INT UNSIGNED NULL,"
        "  chance_pct FLOAT NULL, weight INT UNSIGNED NULL, quantity_min INT UNSIGNED NULL,"
        "  quantity_max INT UNSIGNED NULL, quest_ref_id INT UNSIGNED NULL,"
        "  PRIMARY KEY (loot_table_id, entry_ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE vendor_inventory (id INT UNSIGNED NOT NULL, PRIMARY KEY (id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE vendor_inventory_item (vendor_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
        "  item_id INT UNSIGNED NOT NULL, price_override BIGINT UNSIGNED NULL, limited_count INT UNSIGNED NULL,"
        "  limited_restock_minutes INT UNSIGNED NULL, PRIMARY KEY (vendor_id, ordinal))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // npc_template — the DbNpcStore columns (id, name, vendor_ref_id) + the DbLootTableStore
    // links (loot_table_ref_id, loot_money_*) + the #486 spawn-loader combat columns
    // (level_min, faction, stat_health, stat_mana). Mirrors schema/sql/world/10_npc.sql.
    c.execute(
        "CREATE TABLE npc_template ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL, vendor_ref_id INT UNSIGNED NULL,"
        "  loot_table_ref_id INT UNSIGNED NULL, loot_money_min BIGINT UNSIGNED NULL,"
        "  loot_money_max BIGINT UNSIGNED NULL, level_min SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  faction ENUM('friendly','neutral','hostile') NOT NULL DEFAULT 'neutral',"
        "  stat_health INT UNSIGNED NOT NULL DEFAULT 1, stat_mana INT UNSIGNED NULL,"
        "  stat_armor INT UNSIGNED NULL, stat_damage_min INT UNSIGNED NULL,"
        "  stat_damage_max INT UNSIGNED NULL, stat_attack_speed_ms INT UNSIGNED NOT NULL DEFAULT 2000,"
        "  ai_behavior ENUM('aggressive','defensive','passive') NULL DEFAULT 'defensive',"
        "  ai_aggro_radius_m FLOAT NULL DEFAULT 20, ai_leash_radius_m FLOAT NULL DEFAULT 60,"
        "  move_run_speed_mps FLOAT NULL DEFAULT 7,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE npc_trainer (npc_id INT UNSIGNED NOT NULL, PRIMARY KEY (npc_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE npc_trainer_ability (npc_id INT UNSIGNED NOT NULL, ability_id INT UNSIGNED NOT NULL,"
        "  cost_copper BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "  required_class ENUM('vanguard','runcaller','warden','mender') NULL,"
        "  required_level SMALLINT UNSIGNED NOT NULL DEFAULT 1, PRIMARY KEY (npc_id, ability_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE area (zone_id INT UNSIGNED NOT NULL, poi VARCHAR(64) NOT NULL, name VARCHAR(80) NOT NULL,"
        "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
        "  discovery_radius_m FLOAT NOT NULL DEFAULT 40, discovery_xp INT UNSIGNED NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (zone_id, poi)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // ability — SP2.1 (#691): effects[] rides in effects_json; the per-kind child
    // tables are retired. Columns mirror schema/sql/world/30_ability.sql.
    c.execute(
        "CREATE TABLE ability (id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
        "  school ENUM('physical','fire','frost','nature','shadow','holy','arcane') NOT NULL,"
        "  target ENUM('self','enemy','friendly') NOT NULL, range_m FLOAT NOT NULL DEFAULT 5,"
        "  cast_time_ms INT UNSIGNED NULL DEFAULT 0, cast_channel_ms INT UNSIGNED NULL,"
        "  cooldown_ms INT UNSIGNED NOT NULL DEFAULT 0, triggers_gcd BOOLEAN NOT NULL DEFAULT TRUE,"
        "  resource_type ENUM('mana','rage','energy') NULL, resource_amount INT UNSIGNED NULL,"
        "  effects_json JSON NOT NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // spawn_point — the #486 loader's PASS 1. Columns mirror schema/sql/world/70_spawn.sql.
    c.execute(
        "CREATE TABLE spawn_point (id INT UNSIGNED NOT NULL, zone_ref_id INT UNSIGNED NOT NULL DEFAULT 0,"
        "  npc_id INT UNSIGNED NOT NULL, pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
        "  orientation_deg FLOAT NOT NULL DEFAULT 0, respawn_min INT UNSIGNED NOT NULL DEFAULT 0,"
        "  respawn_max INT UNSIGNED NOT NULL DEFAULT 0, wander_radius_m FLOAT NULL, PRIMARY KEY (id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // race / class — load_world_content also loads the roster (SP2.5 #695,
    // load_db_roster). This test seeds no roster rows and does not assert on it, so
    // the tables are created EMPTY (the loader returns just the compiled fallback).
    // Without the tables load_world_content throws "Table 'race' doesn't exist".
    c.execute("CREATE TABLE race (roster_id TINYINT UNSIGNED NOT NULL, content_id INT UNSIGNED NOT NULL,"
              "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL, PRIMARY KEY (roster_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class (roster_id TINYINT UNSIGNED NOT NULL, content_id INT UNSIGNED NOT NULL,"
              "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
              "  talent_tree_id INT UNSIGNED NULL, PRIMARY KEY (roster_id))"  // SP2.7 #697
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // class_race_limit — load_db_roster also reads this (SP2.6 #696). Empty here
    // (this test does not assert on the roster gate). Without it load_world_content
    // throws "Table 'class_race_limit' doesn't exist".
    c.execute("CREATE TABLE class_race_limit (class_roster_id TINYINT UNSIGNED NOT NULL,"
              "  race_roster_id TINYINT UNSIGNED NOT NULL, PRIMARY KEY (class_roster_id, race_roster_id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // attribute framework — load_world_content also loads it (SP2.4 #694,
    // load_db_attributes). Created EMPTY (this test does not assert on stats); without
    // the tables load_world_content throws "Table 'attribute' doesn't exist".
    c.execute("CREATE TABLE attribute (attr_ref VARCHAR(64) NOT NULL, content_id INT UNSIGNED NOT NULL,"
              "  name VARCHAR(64) NOT NULL, kind ENUM('primary','derived') NOT NULL, PRIMARY KEY (attr_ref))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class_attribute_mod (class_roster_id TINYINT UNSIGNED NOT NULL,"
              "  attr_ref VARCHAR(64) NOT NULL, value INT NOT NULL, PRIMARY KEY (class_roster_id, attr_ref))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE race_attribute_mod (race_roster_id TINYINT UNSIGNED NOT NULL,"
              "  attr_ref VARCHAR(64) NOT NULL, value INT NOT NULL, PRIMARY KEY (race_roster_id, attr_ref))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // equip-gating + role/talent catalogs — load_world_content also reads these
    // (SP2.7 #697: load_db_equip_types / load_db_class_catalog / load_db_talents).
    // Created EMPTY (this test asserts on spawns, not the class kernel); without the
    // tables load_world_content throws "Table 'equip_type' doesn't exist".
    c.execute("CREATE TABLE equip_type (content_id INT UNSIGNED NOT NULL, equip_ref VARCHAR(96) NOT NULL,"
              "  name VARCHAR(64) NOT NULL, category ENUM('armor','weapon') NOT NULL, slot_class VARCHAR(32) NULL,"
              "  PRIMARY KEY (content_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class_usable_equip_type (class_roster_id TINYINT UNSIGNED NOT NULL,"
              "  equip_type_id INT UNSIGNED NOT NULL, list ENUM('armor','weapon') NOT NULL,"
              "  PRIMARY KEY (class_roster_id, equip_type_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE class_role (class_roster_id TINYINT UNSIGNED NOT NULL,"
              "  role ENUM('healer','dps_melee','dps_ranged','tank') NOT NULL,"
              "  PRIMARY KEY (class_roster_id, role)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent (content_id INT UNSIGNED NOT NULL, talent_ref VARCHAR(96) NOT NULL,"
              "  name VARCHAR(64) NOT NULL, rank_max SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
              "  PRIMARY KEY (content_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_grant (talent_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
              "  kind ENUM('ability','buff','debuff') NOT NULL, ability_id INT UNSIGNED NULL,"
              "  attribute_ref VARCHAR(64) NULL, amount INT NULL, modifier ENUM('flat','percent') NULL,"
              "  duration_ms INT UNSIGNED NULL, max_stacks SMALLINT UNSIGNED NULL,"
              "  PRIMARY KEY (talent_id, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_tree (content_id INT UNSIGNED NOT NULL, tree_ref VARCHAR(96) NOT NULL,"
              "  name VARCHAR(64) NOT NULL, description VARCHAR(500) NULL,"
              "  PRIMARY KEY (content_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_tree_tier (talent_tree_id INT UNSIGNED NOT NULL, tier_ordinal SMALLINT UNSIGNED NOT NULL,"
              "  required_points SMALLINT UNSIGNED NOT NULL, PRIMARY KEY (talent_tree_id, tier_ordinal))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE talent_tree_tier_talent (talent_tree_id INT UNSIGNED NOT NULL, tier_ordinal SMALLINT UNSIGNED NOT NULL,"
              "  ordinal SMALLINT UNSIGNED NOT NULL, talent_id INT UNSIGNED NOT NULL,"
              "  PRIMARY KEY (talent_tree_id, tier_ordinal, ordinal)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    // zone / graveyard — load_world_content also resolves the enter-world start-zone
    // spawn (C8 #761, load_start_zone_spawn). Created EMPTY (this test seeds no start
    // zone and does not assert on the enter spawn); the loader returns std::nullopt.
    // Without the tables load_world_content throws "Table 'zone' doesn't exist".
    c.execute("CREATE TABLE zone (id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL,"
              "  level_min SMALLINT UNSIGNED NOT NULL DEFAULT 1, level_max SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
              "  start_zone BOOLEAN NOT NULL DEFAULT FALSE, PRIMARY KEY (id))"
              " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute("CREATE TABLE graveyard (zone_id INT UNSIGNED NOT NULL, ordinal SMALLINT UNSIGNED NOT NULL,"
              "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
              "  orientation_deg FLOAT NOT NULL DEFAULT 0, PRIMARY KEY (zone_id, ordinal))"
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
        "DROP TABLE IF EXISTS graveyard",        "DROP TABLE IF EXISTS zone",
    };
    for (const char* d : drops) c.execute(d);
}

// Seed a friendly quest-giver NPC (100) that gives quest 300, two hostile mobs
// (200/201), and one spawn_point for each. The spawn_point rows are authored in the
// DB's Godot Y-UP convention (schema/sql/world/70_spawn.sql:
// pos_{x,y,z} = position.{x,y,z}
// verbatim, "Y-up, X east, -Z north"): the horizontal plane is (pos_x, pos_z) and
// pos_y is the HEIGHT. A DISTINCT non-zero height is seeded so the test proves the
// #498 axis conversion — load_spawn_points must map the Godot height (pos_y) to the
// server HEIGHT (Position.z) and the Godot planar-north (pos_z) to the server planar
// Y (Position.y), NOT leak the height into planar Y. Server-planar targets: the giver
// at (-320,-320), Dewdrop at (-314,-318), and Sproutcap at (-312,-316) — all well
// inside the 90 m production AoI enter radius (#562) of a session entering at
// (-320,-320), so all must fire an ENTITY_ENTER.
void seed_fixture(db::Connection& c) {
    c.execute(
        "INSERT INTO npc_template (id, name, vendor_ref_id, loot_table_ref_id, loot_money_min, "
        "loot_money_max, level_min, faction, stat_health, stat_mana, stat_armor, "
        "stat_damage_min, stat_damage_max, stat_attack_speed_ms, ai_behavior, "
        "ai_aggro_radius_m, ai_leash_radius_m, move_run_speed_mps) "
        "VALUES (100, 'Pippa Patch', NULL, NULL, NULL, NULL, 1, 'friendly', 100, NULL, 0, "
        "1, 1, 2000, 'passive', 0, 0, 0)");
    c.execute(
        "INSERT INTO npc_template (id, name, vendor_ref_id, loot_table_ref_id, loot_money_min, "
        "loot_money_max, level_min, faction, stat_health, stat_mana, stat_armor, "
        "stat_damage_min, stat_damage_max, stat_attack_speed_ms, ai_behavior, "
        "ai_aggro_radius_m, ai_leash_radius_m, move_run_speed_mps) VALUES "
        "(200, 'Dewdrop Slime', NULL, NULL, NULL, NULL, 1, 'hostile', 100, NULL, 0, "
        "3, 5, 2400, 'aggressive', 8, 18, 3),"
        "(201, 'Sproutcap Scamp', NULL, NULL, NULL, NULL, 2, 'hostile', 140, NULL, 5, "
        "5, 8, 2200, 'aggressive', 10, 22, 4)");
    // Quest 300, given by NPC 100 — so DbNpcStore marks 100 a quest giver and the
    // gossip planner surfaces a quest option (a non-empty menu).
    c.execute(
        "INSERT INTO quest_template (id, name, level, required_level, giver_npc_id, turn_in_npc_id, "
        "reward_xp, reward_money) VALUES (300, 'Thin the Pack', 3, 1, 100, NULL, 100, 50)");
    c.execute(
        "INSERT INTO quest_objective (quest_id, ordinal, type, target_npc_id, item_id, to_npc_id, "
        "zone_ref_id, poi, count) VALUES (300, 0, 'kill', 200, NULL, NULL, NULL, NULL, 3)");
    // spawn_point rows in Godot Y-up (pos_x, pos_y=HEIGHT, pos_z=planar): the giver at
    // planar (-320,-320) height 12.5; the mobs at nearby planar coordinates.
    // respawn 30-45 s.
    c.execute(
        "INSERT INTO spawn_point (id, zone_ref_id, npc_id, pos_x, pos_y, pos_z, orientation_deg, "
        "respawn_min, respawn_max, wander_radius_m) VALUES (1, 20, 100, -320, 12.5, -320, 90, 30, 45, NULL)");
    c.execute(
        "INSERT INTO spawn_point (id, zone_ref_id, npc_id, pos_x, pos_y, pos_z, orientation_deg, "
        "respawn_min, respawn_max, wander_radius_m) VALUES (2, 20, 200, -314, 5, -318, 0, 30, 45, 8)");
    c.execute(
        "INSERT INTO spawn_point (id, zone_ref_id, npc_id, pos_x, pos_y, pos_z, orientation_deg, "
        "respawn_min, respawn_max, wander_radius_m) VALUES (3, 20, 201, -312, 5, -316, 0, 30, 45, 10)");
}

const mw::SpawnPlacement* find_spawn(const std::vector<mw::SpawnPlacement>& spawns,
                                     std::uint32_t npc_id) {
    for (const auto& s : spawns)
        if (s.npc_id == npc_id) return &s;
    return nullptr;
}

}  // namespace

int main() {
    std::printf("worldd content-spawn load + spawn-into-world DB-backed test (#486)\n");

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
                    "spawn-load checks skipped\n");
        return 0;
    }

    try {
        db::Connection conn(p);
        create_tables(conn);
        seed_fixture(conn);

        // ---- 1. LOAD: spawn_point resolved against npc_template ---------------
        mw::WorldContent content = mw::load_world_content(conn);
        check("three spawn placements loaded", content.spawns.size() == 3);

        const mw::SpawnPlacement* giver = find_spawn(content.spawns, kGossipNpc);
        const mw::SpawnPlacement* dewdrop = find_spawn(content.spawns, kDewdropNpc);
        const mw::SpawnPlacement* sproutcap = find_spawn(content.spawns, kSproutcapNpc);
        check("gossip NPC placement loaded", giver != nullptr);
        check("Dewdrop Slime placement loaded", dewdrop != nullptr);
        check("Sproutcap Scamp placement loaded", sproutcap != nullptr);
        if (giver) {
            check("gossip NPC name resolved from npc_template", giver->name == "Pippa Patch");
            check("gossip NPC is friendly", giver->stats.faction == mw::Faction::kFriendly);
            check("friendly NPC authored passive combat profile loaded",
                  giver->behavior == mw::CreatureBehavior::kPassive &&
                      giver->damage_min == 1 && giver->damage_max == 1 &&
                      giver->attack_speed_ms == 2000);
            // AXIS-CORRECT (#498): the authored Godot Y-up row (pos_x=-320, pos_y=12.5
            // height, pos_z=-320 planar) must load as server Z-up (x=-320 planar, y=-320
            // planar from pos_z, z=12.5 height from pos_y). Critically pos.y == -320 (the
            // authored planar-north pos_z), NOT 12.5 (the authored HEIGHT pos_y) — the
            // height must not leak into planar Y.
            check("gossip NPC position is axis-correct (Godot Y-up -> server Z-up)",
                  giver->pos.x == -320.0f && giver->pos.y == -320.0f && giver->pos.z == 12.5f);
        }
        if (dewdrop) {
            check("Dewdrop authored combat profile loaded",
                  dewdrop->stats.level == 1 && dewdrop->stats.max_health == 100 &&
                      dewdrop->stats.armor == 0 && dewdrop->damage_min == 3 &&
                      dewdrop->damage_max == 5 && dewdrop->attack_speed_ms == 2400 &&
                      dewdrop->behavior == mw::CreatureBehavior::kAggressive &&
                      dewdrop->aggro_radius_m == 8.0f && dewdrop->leash_radius_m == 18.0f &&
                      dewdrop->run_speed_mps == 3.0f);
            check("Dewdrop position is axis-correct (Godot Y-up -> server Z-up)",
                  dewdrop->pos.x == -314.0f && dewdrop->pos.y == -318.0f && dewdrop->pos.z == 5.0f);
        }
        if (sproutcap) {
            check("Sproutcap authored combat profile loaded",
                  sproutcap->stats.level == 2 && sproutcap->stats.max_health == 140 &&
                      sproutcap->stats.armor == 5 && sproutcap->damage_min == 5 &&
                      sproutcap->damage_max == 8 && sproutcap->attack_speed_ms == 2200 &&
                      sproutcap->behavior == mw::CreatureBehavior::kAggressive &&
                      sproutcap->aggro_radius_m == 10.0f && sproutcap->leash_radius_m == 22.0f &&
                      sproutcap->run_speed_mps == 4.0f);
        }

        // ---- 2. INSTALL: spawn into the live world ----------------------------
        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, mw::WorldServerConfig{});
        world.install_spawns(content.spawns);
        check("all spawns exist in the map tick", world.map_creature_count() == 3);
        check("all spawns are AoI entities", world.world_state().world_entity_count() == 3);

        // ---- 3. VISIBILITY: a session entering AoI sees the nearby spawns ------
        // Capture every s2c frame the relay sends the entering session. After the #498
        // axis conversion the three spawns sit at nearby server-planar coordinates, well
        // within the AoI enter radius of a session spawning at (64,64), so all fire an
        // ENTITY_ENTER on enter. (Before the fix the authored height leaked into planar
        // Y — the giver landed at planar (-320,12.5), ~332 m away, outside the 90 m enter
        // radius — so no ENTITY_ENTER arrived and the greybox was empty.)
        std::vector<std::pair<mn::Opcode, std::vector<std::uint8_t>>> frames;
        mw::EntityIdentity self;
        self.entity_guid = 4242;  // a distinct player guid
        self.type_id = 2;
        self.char_class = 2;
        mw::Position spawn;
        spawn.x = -320.0f; spawn.y = -320.0f; spawn.z = 0.0f;  // Zone-01 centre (#562)
        world.world_state().enter(
            self, spawn,
            [&frames](mn::Opcode op, const std::vector<std::uint8_t>& payload) {
                frames.emplace_back(op, payload);
                return true;
            });

        // Decode every ENTITY_ENTER and match it to a spawn (guid in the world-entity
        // band; name + level from the resolved npc_template vitals).
        bool saw_gossip_enter = false, saw_dewdrop_enter = false, saw_sproutcap_enter = false;
        std::uint64_t gossip_guid = 0;
        for (const auto& [op, payload] : frames) {
            if (op != mn::Opcode::ENTITY_ENTER) continue;
            const auto* ee = fb::GetRoot<mn::EntityEnter>(payload.data());
            if (ee == nullptr) continue;
            const std::string name = ee->name() ? ee->name()->str() : std::string();
            if (name == "Pippa Patch" && ee->level() == 1 && ee->max_health() == 100) {
                saw_gossip_enter = true;
                gossip_guid = ee->entity_guid();
            }
            if (name == "Dewdrop Slime" && ee->max_health() == 100) saw_dewdrop_enter = true;
            if (name == "Sproutcap Scamp" && ee->max_health() == 140) saw_sproutcap_enter = true;
        }
        check("entering AoI delivers ENTITY_ENTER for the spawned gossip NPC (with vitals+name)",
              saw_gossip_enter);
        check("entering AoI delivers ENTITY_ENTER for Dewdrop Slime", saw_dewdrop_enter);
        check("entering AoI delivers ENTITY_ENTER for Sproutcap Scamp", saw_sproutcap_enter);
        check("spawned entity guid is in the world-entity band (not a template id)",
              gossip_guid >= mw::kWorldEntityGuidBase);

        // ---- 4. INTERACTABILITY ----------------------------------------------
        // GOSSIP_HELLO resolves: the clicked entity's wire guid maps back to its
        // npc_template id, so npc_store().find(that id) serves the gossip NPC's content.
        if (gossip_guid != 0) {
            auto tmpl = world.world_state().npc_template_for_guid(gossip_guid);
            check("spawned gossip guid resolves to its npc_template id (GOSSIP_HELLO seam)",
                  tmpl.has_value() && *tmpl == kGossipNpc);
            // GOSSIP_HELLO does exactly this: npc_store().find(resolved template id).
            const meridian::npc::NpcDef* def =
                tmpl ? content.npcs->find(static_cast<meridian::npc::NpcId>(*tmpl)) : nullptr;
            check("resolved NPC def is the quest-giver (gossip menu is non-empty)",
                  def != nullptr && !def->quests.empty() && def->quests[0].quest_id == kGiverQuest);

            // The spawned entity is reachable by guid for a cast/kill (targetable).
            const mw::Unit* u = world.world_state().unit_for_guid(gossip_guid);
            check("spawned entity is reachable by guid (targetable Unit)",
                  u != nullptr && u->is_alive() && u->faction() == mw::Faction::kFriendly);
        } else {
            check("spawned gossip guid resolves to its npc_template id (GOSSIP_HELLO seam)", false);
            check("resolved NPC def is the quest-giver (gossip menu is non-empty)", false);
            check("spawned entity is reachable by guid (targetable Unit)", false);
        }

        // A hostile creature is present for a kill objective (in the map tick's AI set).
        bool hostile_present = false;
        for (const auto& s : content.spawns)
            if (s.stats.faction == mw::Faction::kHostile) hostile_present = true;
        check("a hostile creature is present for a kill objective", hostile_present);

        // ---- 5. AUTHORED COMBAT: DB profiles execute in the live MapTick -------
        auto combat_def = [](const mw::SpawnPlacement& sp) {
            mw::CreatureSpawnDef d;
            d.template_id = sp.npc_id;
            d.level = sp.stats.level;
            d.faction = sp.stats.faction;
            d.authored_stats = sp.stats;
            d.behavior = sp.behavior;
            d.damage_min = sp.damage_min;
            d.damage_max = sp.damage_max;
            d.attack_speed_ms = sp.attack_speed_ms;
            d.home = sp.pos;
            d.aggro_base_radius = sp.aggro_radius_m;
            d.leash_radius = sp.leash_radius_m;
            d.move_speed = sp.run_speed_mps;
            d.respawn_ms = sp.respawn_min * 1000u;
            return d;
        };
        auto profile_attacks = [&](const mw::SpawnPlacement& sp, std::uint64_t seed) {
            mw::AbilityStore abilities = mw::load_placeholder_ability_store();
            mw::MapTick tick(abilities, seed, /*dt_ms=*/100);
            mw::UnitStats player_stats;
            player_stats.level = 1;
            player_stats.max_health = 1000;
            player_stats.armor = 100;  // proves the shared armor path is exercised
            player_stats.faction = mw::Faction::kPlayer;
            tick.add_player(/*guid=*/4242, sp.pos, player_stats);
            tick.add_creature(combat_def(sp));
            tick.advance(/*ticks=*/60);  // covers >2 authored attack periods
            const mw::Unit* player = tick.unit_for_guid(4242);
            return player != nullptr && player->health() < player->max_health() &&
                   tick.log_text().find("basic_attack attacker=") != std::string::npos;
        };
        check("DB-backed Dewdrop Slime executes authored basic attacks",
              dewdrop != nullptr && profile_attacks(*dewdrop, 784));
        check("DB-backed Sproutcap Scamp executes authored basic attacks",
              sproutcap != nullptr && profile_attacks(*sproutcap, 785));

        if (giver) {
            mw::AbilityStore abilities = mw::load_placeholder_ability_store();
            mw::MapTick tick(abilities, /*seed=*/786, /*dt_ms=*/100);
            mw::UnitStats player_stats;
            player_stats.max_health = 100;
            player_stats.faction = mw::Faction::kPlayer;
            tick.add_player(/*guid=*/4242, giver->pos, player_stats);
            tick.add_creature(combat_def(*giver));
            tick.advance(/*ticks=*/60);
            const mw::Unit* player = tick.unit_for_guid(4242);
            check("DB-backed friendly passive NPC never attacks",
                  player != nullptr && player->health() == 100 &&
                      tick.log_text().find("basic_attack attacker=") == std::string::npos);
        }

        drop_tables(conn);
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all spawn-load checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d spawn-load check(s) failed\n", g_fail);
    return 1;
}
