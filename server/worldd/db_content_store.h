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
#include <unordered_map>
#include <vector>

#include "meridian/db/connection.h"

#include "ability_store.h"    // meridian::worldd::AbilityStore — DB-loaded ability catalog (#481)
#include "area_triggers.h"    // meridian::worldd::TriggerVolume — DB-loaded POI volumes (#398)
#include "item_template.h"    // meridian::items::TemplateStore / ItemTemplate
#include "loot_table.h"       // meridian::loot::LootTableStore / LootTable
#include "npc_def.h"          // meridian::npc::NpcStore / NpcDef
#include "quest_def.h"        // meridian::worldd::QuestStore / QuestDef
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
    // The authored ability catalog (ability / ability_effect / ability_effect_stat_mod
    // rows), keyed by IF-9 numeric id (#481). Installed onto the live cast path via
    // WorldServer::set_abilities() so a client casting an authored id (minor_healing=1)
    // resolves against real content instead of the placeholder store's synthetic ids.
    std::unique_ptr<AbilityStore>     abilities;
};

// Load the authored ability catalog (ability + ability_effect + ability_effect_stat_mod)
// from the world DB into an AbilityStore keyed by IF-9 numeric id (#481). The read path
// #390 left open — installed on the live cast path in place of the M1 placeholder store.
// Throws meridian::db::DbError on a query failure (same fail-fast policy as the others).
AbilityStore load_db_ability_store(db::Connection& world_db);

// Load every content store from the (already-boot-verified) world DB connection.
// Throws meridian::db::DbError on a query failure — the caller treats a load fault
// like the boot-check connect fault (fail-fast; a half-loaded world is not served).
WorldContent load_world_content(db::Connection& world_db);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_DB_CONTENT_STORE_H
