// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB CONTENT LOAD layer (issue #390, epic #20). The read path that
// closes the gap between the built M1 game-loop systems (#371 quests, #369 loot,
// #372 npc gossip/trainer, #370 vendors, all live-dispatched by #388) and the
// AUTHORED content compiled into the world DB by mcc emit-sql (IF-4, #120).
//
// WHAT THIS IS: DB-backed implementations of the four M1 content-store SEAMS —
//   * meridian::worldd::QuestStore       (quest_def.h)
//   * meridian::npc::NpcStore            (npc/npc_def.h)
//   * meridian::loot::LootTableStore     (loot/loot_table.h, keyed by creature)
//   * meridian::vendor::VendorCatalog    (vendor/vendor_catalog.h)
// plus meridian::items::TemplateStore (item/item_template.h) so a DB swap of the
// item-referencing stores stays referentially coherent (a DB vendor listing / quest
// reward / loot entry names a real item id, which resolves against a DB-loaded
// template, not the disjoint placeholder id band).
//
// Each store loads ALL of its rows ONCE at boot into an in-memory map behind the
// SAME abstract seam the placeholder store implements, then answers the identical
// queries from memory — so the #388 dispatch handlers and the seam interfaces are
// UNCHANGED; only the concrete store swaps (placeholder for a DB-free unit test, a
// DB store when a world DB is configured). M1 content is small, so a full load at
// boot (no lazy/streaming) is fine (SAD §4.3 "the world DB is read-only, replaced
// wholesale nightly").
//
// SCOPE (schema fidelity): the load maps each authored world-DB row set onto the
// existing seam struct. Every M1 seam field now has a world-DDL home:
//   * NPC TRAINER abilities (NpcDef::is_trainer / trainer_abilities) load from the
//     npc_trainer / npc_trainer_ability tables (#392): an NPC with any taught-ability
//     row is a trainer, and each row carries the ability ref + copper cost + class /
//     level gate the live #388 TRAINER_LIST/TRAINER_LEARN path reads. So a DB-loaded
//     trainer works end-to-end from authored content. See db_content_store.cpp.
//
// PARAMETERIZED SQL ONLY (CONTRIBUTING.md backend rule): every query binds through
// meridian::db prepared-statement parameters; no value is ever concatenated in.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL
// schema/sql/world/*.sql + the seam headers; no GPL/AGPL/CMaNGOS/TrinityCore/leaked
// source consulted).

#ifndef MERIDIAN_WORLDD_DB_CONTENT_STORE_H
#define MERIDIAN_WORLDD_DB_CONTENT_STORE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "meridian/db/connection.h"

#include "ability_store.h"    // meridian::worldd::AbilityStore — DB-loaded ability catalog (#481)
#include "area_triggers.h"    // meridian::worldd::TriggerVolume — DB-loaded POI volumes (#398)
#include "class_kernel.h"     // meridian::worldd::EquipTypeCatalog / ClassCatalog — equip-gating (#697)
#include "effective_stats.h"  // meridian::worldd::AttributeCatalog — DB-loaded attribute framework (#694)
#include "talent_catalog.h"   // meridian::worldd::TalentCatalog — DB-loaded talents/trees (#697)
#include "combat_unit.h"      // meridian::worldd::UnitStats / Faction / Position (spawn stats, #486)
#include "item_template.h"    // meridian::items::TemplateStore / ItemTemplate
#include "loot_table.h"       // meridian::loot::LootTableStore / LootTable
#include "npc_def.h"          // meridian::npc::NpcStore / NpcDef
#include "quest_def.h"        // meridian::worldd::QuestStore / QuestDef
#include "roster.h"           // meridian::characters::Roster — DB-loaded playable roster (#695)
#include "vendor_catalog.h"   // meridian::vendor::VendorCatalog / VendorListing

namespace meridian::worldd {

// --- DB-backed item template store (ITM-01 seam, item_template.h) -------------
// Loads item_template (+ item_stat + weapon_* + prices) into memory. Provided so a
// DB swap of the vendor/quest/loot stores (which reference item ids) resolves those
// ids against real templates rather than the disjoint placeholder id band.
class DbTemplateStore : public items::TemplateStore {
public:
    // Load every item_template row (and its item_stat children) from `world_db`.
    explicit DbTemplateStore(db::Connection& world_db);
    const items::ItemTemplate* find(std::uint32_t template_id) const override;

    // Every loaded template id, ascending (tests / dev tooling).
    std::vector<std::uint32_t> ids() const;

private:
    std::unordered_map<std::uint32_t, items::ItemTemplate> by_id_;
};

// --- DB-backed quest store (QST-01 seam, quest_def.h) ------------------------
// Loads quest_template + quest_objective + quest_prereq + quest_reward into QuestDef.
class DbQuestStore : public QuestStore {
public:
    explicit DbQuestStore(db::Connection& world_db);
    const QuestDef* find(QuestId quest_id) const override;
    std::vector<QuestId> ids() const override;

private:
    std::unordered_map<QuestId, QuestDef> by_id_;
};

// --- DB-backed NPC store (NPC-01/02 seam, npc_def.h) -------------------------
// Loads npc_template into NpcDef: id, name, the vendor role FLAG (vendor_ref_id set),
// the quest giver/turn-in participation (cross-referenced from quest_template's
// giver_npc_id / turn_in_npc_id — the DDL keeps that link on the quest side), and the
// trainer role (is_trainer + trainer_abilities) from npc_trainer_ability (#392): a
// taught-ability row makes the NPC a trainer and carries the cost + class/level gate.
class DbNpcStore : public npc::NpcStore {
public:
    explicit DbNpcStore(db::Connection& world_db);
    const npc::NpcDef* find(npc::NpcId npc_id) const override;
    std::vector<npc::NpcId> ids() const override;

private:
    std::unordered_map<npc::NpcId, npc::NpcDef> by_id_;
};

// --- DB-backed loot-table store (ITM-02 seam, loot_table.h) ------------------
// Keyed by CREATURE template id (the seam's key): npc_template.loot_table_ref_id
// links a creature to its loot_table, so this loads npc_template's loot links and the
// loot_table / loot_group / loot_entry rows, assembling one LootTable per creature.
// The creature's own loot_money_min/max is added to the table money range (D-25
// additive). Top-level independent entries become single-entry groups (loot_table.h:
// "independent single-item drops are modelled as a group with one entry").
class DbLootTableStore : public loot::LootTableStore {
public:
    explicit DbLootTableStore(db::Connection& world_db);
    const loot::LootTable* find(std::uint32_t creature_template_id) const override;

    // Every creature template id that carries loot, ascending (tests / tooling).
    std::vector<std::uint32_t> ids() const;

private:
    std::unordered_map<std::uint32_t, loot::LootTable> by_creature_;
};

// --- DB-backed vendor catalog (ECO-01 seam, vendor_catalog.h) ----------------
// Loads vendor_inventory + vendor_inventory_item into per-vendor listing lists.
class DbVendorCatalog : public vendor::VendorCatalog {
public:
    explicit DbVendorCatalog(db::Connection& world_db);
    const std::vector<vendor::VendorListing>* listings(std::uint32_t vendor_id) const override;

    // Every loaded vendor id, ascending (tests / tooling).
    std::vector<std::uint32_t> ids() const;

private:
    std::unordered_map<std::uint32_t, std::vector<vendor::VendorListing>> by_vendor_;
};

// --- DB-backed area-trigger / POI volumes (#398, WLD-03) ---------------------
// Build one discovery TriggerVolume per authored `area` (POI) row: the pos_x/y/z +
// discovery_radius_m FLOAT columns (readable since the #393 FLOAT-read fix) become
// an axis-aligned box centred on the POI, and area.zone_id + area.poi become the
// volume's area_id + poi — the SAME join key a kExplore quest objective matches on
// (QST-01 #396: a discovery crossing credits the explore objective whose
// (zone_id, poi) == (this area_id, this poi)). So a player crossing a DB-loaded POI
// volume fires an ENTER carrying the authored poi, and on_explore(zone_id, poi)
// credits the matching objective against real content — no synthetic volume needed.
//
// Membership is a point-in-box test on (x, y) with the z axis spanning full range
// (the M1 flat map is a single plane; z is ignored, mirroring the placeholder set).
// Ids are assigned 1..N in (zone_id, poi) load order (the `area` PK has no numeric
// id) — stable for the per-character occupancy bookkeeping, since the whole set is
// loaded once at boot.
std::vector<TriggerVolume> load_area_trigger_volumes(db::Connection& world_db);

// --- DB-backed spawn placements (NPC-01 spawn seam, #486) --------------------
// One authored `spawn_point` row RESOLVED against its `npc_template`: the placed
// creature/NPC the world spawns into the live map at boot (previously the deferred
// "#28 content spawns" seam — DbNpcStore #390 loaded the TEMPLATES but nothing read
// spawn_point, so no live entity existed). Carries everything MapTick::add_creature
// (existence/AI/kill) AND the AoI relay (ENTITY_ENTER with #430 vitals + name) need:
// the resolved combat stats/faction/level (from npc_template), the position (from
// spawn_point), and the display name. Respawn timing / wander are carried for the
// minimal M1 AI (respawn is #346-#348; the point is the entity EXISTS + is visible +
// interactable). `stats.faction` distinguishes a friendly/quest-giver NPC (targetable
// + GOSSIP_HELLO) from a hostile creature (a kill objective).
struct SpawnPlacement {
    std::uint32_t npc_id = 0;          // spawn_point.npc_id == npc_template.id (the target of GOSSIP_HELLO)
    Position pos;                      // spawn_point.pos_{x,y,z} (zone-local m); AoI position + spawn_home
    float orientation_deg = 0.0f;      // spawn_point.orientation_deg [0,360)
    UnitStats stats;                   // resolved from npc_template: level, max_health, resource, faction
    std::string name;                  // npc_template.name (the #430 vitals + nameplate name)
    std::uint32_t respawn_min = 0;     // spawn_point.respawn_min (seconds)
    std::uint32_t respawn_max = 0;     // spawn_point.respawn_max (seconds)
    std::optional<float> wander_radius_m;  // spawn_point.wander_radius_m (NULL when it patrols instead)
};

// Load every `spawn_point` row, resolving each against its `npc_template` (name +
// stats + faction + level), into a placement the boot path spawns into the live
// world (#486). Parameterized SQL; the npc_template columns are only touched when at
// least one spawn_point row exists, so a world DB with an EMPTY spawn_point table
// (the DB-content unit tests) loads zero spawns without requiring the extended
// npc_template columns. Throws meridian::db::DbError on a query failure (fail-fast,
// same policy as the other loaders).
std::vector<SpawnPlacement> load_spawn_points(db::Connection& world_db);

// --- DB-backed enter-world spawn (C8 enter-as-chibi, #761) -------------------
// The character's ENTER-WORLD spawn point, resolved from the loaded pack: the
// realm's START ZONE (`zone.start_zone = TRUE`) first graveyard (`graveyard`
// ordinal 0). This is exactly zone.schema.yaml's contract — "start_zone: spawn
// point = first graveyard" (the graveyards[] first entry is the zone default and,
// for a start zone, the enter-world spawn). The realm world DB is built SINGLE-PACK
// (scripts/content-build.sh `emit-sql --pack <ns>`), so it ships exactly one start
// zone — the theme's own (the chibi realm's Sprout Meadow, graveyard at origin);
// core's is not present. `pos` is already converted to the worldd Z-up runtime
// frame (the DB mirrors Godot Y-up content, common.defs "Y-up, X east, -Z north" —
// the same #498 conversion load_spawn_points applies) and carries the graveyard
// facing as its orientation. Replaces the D-11 PLACEHOLDER spawn (movement::
// kZoneSpawnXY, the Zone-01 flat-ground centre) the enter-world handler used before.
struct EnterSpawn {
    std::uint32_t zone_id = 0;   // zone.id of the resolved start zone (for logging)
    Position pos;                // graveyard[0] position + facing, in the Z-up runtime frame
};

// Resolve the enter-world spawn from the world DB (#761): the start zone's first
// graveyard. Returns std::nullopt when the world DB ships NO start zone with a
// graveyard (e.g. a content-less / degraded world DB, or a pack that authors no
// start_zone) — the enter-world handler then keeps the movement::kZoneSpawnXY
// placeholder. Deterministic (lowest start-zone id wins if a merged DB somehow
// carries more than one; the realm DB is single-pack so there is exactly one).
// Throws meridian::db::DbError on a query failure (fail-fast, same policy as the
// other loaders).
std::optional<EnterSpawn> load_start_zone_spawn(db::Connection& world_db);

// --- The loaded world content bundle -----------------------------------------
// Owns one of each DB-backed store, loaded from the world DB at boot. Held for the
// process lifetime (main() owns it) so the pointers install_content_stores() /
// WorldServer::set_loot_tables() hand to the seams stay valid for every served
// connection. Move-only (owns unique_ptrs).
struct WorldContent {
    std::unique_ptr<DbTemplateStore>  items;
    std::unique_ptr<DbVendorCatalog>  vendor;
    std::unique_ptr<DbQuestStore>     quests;
    std::unique_ptr<DbNpcStore>       npcs;
    std::unique_ptr<DbLootTableStore> loot;
    // The authored POI discovery volumes (area rows) the map tick evaluates against
    // player positions — carries the real `poi` so explore objectives credit against
    // authored content (#398). Empty when the world DB has no `area` rows.
    std::vector<TriggerVolume>        area_triggers;
    // The authored ability catalog (ability rows; the effects[] recipe rides in the
    // generic effects_json payload since SP2.1), keyed by IF-9 numeric id (#481).
    // Installed onto the live cast path via
    // WorldServer::set_abilities() so a client casting an authored id (minor_healing=1)
    // resolves against real content instead of the placeholder store's synthetic ids.
    std::unique_ptr<AbilityStore>     abilities;
    // The runtime playable roster (SP2.5 #695) — the `race`/`class` rows loaded from
    // pack data, MERGED with the compiled fallback (the entries not yet authorable in
    // the pack). Installed onto the CHAR_CREATE validation path via
    // WorldServer::set_roster() so a create validates against pack data instead of the
    // retired compiled enum. Always populated by load_world_content (the fallback
    // alone is non-empty), so std::optional only marks "a world DB was loaded".
    std::optional<meridian::characters::Roster> roster;
    // The attribute framework (SP2.4 #694) — the base attribute vocabulary + the
    // per-class/per-race attribute_mods loaded from the `attribute` /
    // `class_attribute_mod` / `race_attribute_mod` tables. Consumed by the kernel's
    // EffectiveStats computation (effective_stats.h): a character's effective stats =
    // base + class mods + race mods + the live buff/debuff aura layer. Always
    // populated by load_world_content (empty tables load an empty catalog).
    AttributeCatalog attributes;
    // The equip-type catalog (SP2.7 #697) — the armor/weapon type vocabulary
    // (equip_type rows) the class kernel gates equipping against. Empty when the
    // world DB has no equip_type rows.
    EquipTypeCatalog equip_types;
    // The per-class equip-gating + role/talent rules (SP2.7 #697) — usable armor/
    // weapon types, role(s), and talent tree id, loaded from `class` +
    // `class_usable_equip_type` + `class_role`, keyed by roster_id. Consumed by the
    // kernel's gate_equip / threat_multiplier hooks.
    ClassCatalog classes;
    // The talents + talent trees (SP2.7 #697) — loaded from `talent` /
    // `talent_grant` / `talent_tree` / `talent_tree_tier` / `talent_tree_tier_talent`.
    // Consumed by apply_talents (talent grants -> usable abilities + passive
    // effective-stat deltas). Empty when the world DB authors no talents.
    TalentCatalog talents;
    // The authored spawn placements (spawn_point rows resolved against npc_template,
    // #486). Spawned into the live world at boot via WorldServer::install_spawns() so
    // the seeded quest-givers/creatures EXIST, are AoI-visible (ENTITY_ENTER), and are
    // interactable (GOSSIP_HELLO / kill objectives). Empty when the world DB has no
    // spawn_point rows.
    std::vector<SpawnPlacement>       spawns;
    // The enter-world spawn (C8, #761): the start zone's first graveyard, resolved
    // from the loaded pack (load_start_zone_spawn). Installed onto the enter-world
    // path via WorldServer::set_enter_spawn so a character spawns at its realm's
    // start zone (the chibi realm's Sprout Meadow graveyard) instead of the D-11
    // placeholder. std::nullopt when the world DB ships no start zone with a
    // graveyard — the enter-world handler then keeps the movement::kZoneSpawnXY
    // placeholder (DB-less / degraded / no-start-zone).
    std::optional<EnterSpawn>         enter_spawn;
};

// Load the authored ability catalog (ability rows; effects[] deserialized from the
// generic effects_json payload since SP2.1) from the world DB into an AbilityStore
// keyed by IF-9 numeric id (#481). The read path #390 left open — installed on the
// live cast path in place of the M1 placeholder store.
// Throws meridian::db::DbError on a query failure (same fail-fast policy as the others).
AbilityStore load_db_ability_store(db::Connection& world_db);

// Load the playable roster from the world DB (SP2.5 #695). Starts from the compiled
// fallback (meridian::characters::Roster::compiled_fallback() — the entries not yet
// authorable in the pack) and MERGES every `race` / `class` row on top (keyed by
// roster_id, the canonical character.race/class id), so a pack entry supersedes a
// same-id fallback entry (the pack is the source of truth). Throws
// meridian::db::DbError on a query failure (same fail-fast policy as the others).
meridian::characters::Roster load_db_roster(db::Connection& world_db);

// Load the attribute framework from the world DB (SP2.4 #694): the base attribute
// vocabulary (`attribute` table — ref/name/kind) plus the per-class/per-race
// `attribute_mods` (`class_attribute_mod` / `race_attribute_mod`, keyed by roster_id)
// into an AttributeCatalog the EffectiveStats computation consumes. Empty tables
// load an empty catalog (no throw). Throws meridian::db::DbError on a query failure
// (same fail-fast policy as the others).
AttributeCatalog load_db_attributes(db::Connection& world_db);

// Load the equip-type catalog from the world DB (SP2.7 #697): the `equip_type` rows
// (numeric id + ref + name + category + slot_class) into an EquipTypeCatalog the
// class kernel gates equipping against. Empty tables load an empty catalog (no
// throw). Throws meridian::db::DbError on a query failure.
EquipTypeCatalog load_db_equip_types(db::Connection& world_db);

// Load the per-class equip-gating + role rules from the world DB (SP2.7 #697): the
// `class` identity + `class_usable_equip_type` (usable armor/weapon types) +
// `class_role` (role set) rows into a ClassCatalog keyed by roster_id, plus the
// class.talent_tree_id link. Empty tables load an empty catalog. Throws
// meridian::db::DbError on a query failure.
ClassCatalog load_db_class_catalog(db::Connection& world_db);

// Load the talents + talent trees from the world DB (SP2.7 #697): `talent` +
// `talent_grant` + `talent_tree` + `talent_tree_tier` + `talent_tree_tier_talent`
// into a TalentCatalog the apply_talents hook consumes. Empty tables load an empty
// catalog. Throws meridian::db::DbError on a query failure.
TalentCatalog load_db_talents(db::Connection& world_db);

// Load every content store from the (already-boot-verified) world DB connection.
// Throws meridian::db::DbError on a query failure — the caller treats a load fault
// like the boot-check connect fault (fail-fast; a half-loaded world is not served).
WorldContent load_world_content(db::Connection& world_db);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_DB_CONTENT_STORE_H
