// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — the loot TABLE model + the read-only loot-table datastore seam
// (ITM-02; server PRD §4-M1 "loot generation from loot tables at kill time …
// quest-item drop rates conditioned on quest state"; issue #369).
//
// Provenance / clean-room basis (CONTRIBUTING.md — no GPL/AGPL/CMaNGOS/
// TrinityCore/leaked loot tables consulted):
//   * The loot MODEL (a creature's loot table → weighted groups → entries, plus a
//     money range) is designed from OUR docs: server SAD §4.4 names the world-DB
//     table family `loot_table`/`loot_entry`/`loot_group`, and server PRD §4-M1
//     (ITM-02) requires "weighted drops + quantity ranges + money" with "quest-
//     item drop rates conditioned on quest state". This header is the server-side
//     in-memory view of one compiled loot table; the FIELD NAMES mirror that DDL
//     family, nothing else.
//   * The TEMPLATE vs INSTANCE split lives in meridian::items (item_template.h);
//     a LootEntry references an ItemTemplate by numeric id (IF-9), never inlines
//     item data — the item model is the single source of truth for what an item
//     IS; a loot table only says what DROPS and how often.
//
// DATASTORE SEAM (ITM-02, mcc #28): loot tables are a read-only content artifact,
// exactly like item templates (item_template.h) and abilities (ability_store.h).
// At M1 the real pipeline (mcc #28 compiles loot.*.yaml → the world DB
// `loot_table` family, replaced wholesale nightly) is NOT built. This header
// exposes an ABSTRACT `LootTableStore` (the seam, keyed by a creature's template
// id) plus a `PlaceholderLootTableStore` (a small ORIGINAL M1 set, loot_table.cpp)
// so loot rolls have real data to operate on. When mcc #28 lands, a
// `WorldDbLootTableStore` implements the SAME seam over the world DB and the
// placeholder set is dropped — no roll / session code changes.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "item_template.h"  // meridian::items::Copper

namespace meridian::loot {

// The basis-point scale for every loot probability roll (a d10000). A chance of
// `n` basis points means n/10000 — 10000 = always, 0 = never. Named so the roll
// (loot_roll.h) and the tables (loot_table.cpp) agree on one scale.
inline constexpr std::uint32_t kLootRollScale = 10000;

// One candidate drop within a loot group — a reference to an item template plus
// the quantity range it drops in and its selection weight. Mirrors the world-DB
// `loot_entry` shape (server SAD §4.4).
struct LootEntry {
    std::uint32_t item_template_id = 0;  // -> meridian::items ItemTemplate (IF-9)
    std::uint32_t min_qty = 1;           // inclusive low of the stack size
    std::uint32_t max_qty = 1;           // inclusive high (>= min_qty)
    std::uint32_t weight = 1;            // relative selection weight within the group (>0)

    // Quest gate (ITM-02): if non-zero, this drop is a QUEST item that is only
    // visible/lootable by a player who currently has quest `required_quest_id`
    // (server PRD §4-M1 "quest-item drop rates conditioned on quest state"). The
    // ROLL still rolls it (deterministically, seed-only); the loot SESSION
    // (loot_session.h) enforces per-looter quest eligibility at loot time. 0 = a
    // normal (non-quest) drop, visible to every eligible looter.
    std::uint32_t required_quest_id = 0;
};

// A weighted drop group (world-DB `loot_group`, server SAD §4.4). With probability
// `chance_bp / kLootRollScale` the group drops exactly ONE of its entries, chosen
// by relative `weight`. Independent single-item drops are modelled as a group with
// one entry (chance_bp = that item's drop chance) — so one uniform mechanism
// covers both "each item rolls on its own" and "pick one of these by weight".
struct LootGroup {
    std::uint32_t chance_bp = 0;        // P(this group drops anything), basis points
    std::vector<LootEntry> entries;     // candidates; exactly one is chosen on a hit
};

// A creature's complete loot table (world-DB `loot_table`, server SAD §4.4): a
// money range (whole copper, ECO-01) plus the weighted drop groups. A roll
// (loot_roll.h) draws money uniformly in [money_min, money_max] and rolls each
// group independently.
struct LootTable {
    items::Copper money_min = 0;   // inclusive low of the copper drop (>= 0)
    items::Copper money_max = 0;   // inclusive high (>= money_min); 0/0 = no money
    std::vector<LootGroup> groups;
};

// --- Loot-table datastore seam (ITM-02, mcc #28) -----------------------------
// The read-only source of loot tables, keyed by the CREATURE template id whose
// death rolls it (server SAD §4.4 npc_template → loot_table link). Roll/session
// code depends ONLY on this interface, never on where tables come from. M1 wires
// the placeholder implementation below; mcc #28 later adds a world-DB
// implementation of the SAME interface and the placeholder set is deleted.
class LootTableStore {
public:
    virtual ~LootTableStore() = default;

    // The loot table a creature of `creature_template_id` rolls on death, or
    // nullptr if that creature has no loot (a valid, common case — not every
    // creature drops). The pointer is owned by the store, valid for its lifetime.
    virtual const LootTable* find(std::uint32_t creature_template_id) const = 0;
};

// Reserved id range for the M1 PLACEHOLDER creature templates that carry loot.
// Deliberately distinct from the combat/AI test creatures (template_id 1) so the
// combat golden scenarios (which spawn template_id 1) roll NO loot and their
// byte-stable streams are unaffected — a stray placeholder never masquerades as a
// real compiled creature once mcc #28 lands.
inline constexpr std::uint32_t kPlaceholderCreatureIdBase = 800000;

// Named placeholder creatures that carry loot (loot_table.cpp). Distinct so tests
// and the map tick can spawn the exact archetype they want:
//   * kCreatureWolf     — a common mob: guaranteed pocket money + a common weighted
//                         drop group (potion/ore) + a rare weapon drop.
//   * kCreatureCourier  — carries the quest item: a normal common drop plus a
//                         QUEST-gated drop that only players on kPlaceholderQuestId
//                         may see/loot.
inline constexpr std::uint32_t kCreatureWolf = kPlaceholderCreatureIdBase + 1;
inline constexpr std::uint32_t kCreatureCourier = kPlaceholderCreatureIdBase + 2;

// The quest whose acceptance a player must hold to see/loot kCreatureCourier's
// quest-gated drop (a stand-in for a real quest id; quests are QST-01 / mcc #28).
inline constexpr std::uint32_t kPlaceholderQuestId = 700042;

// The M1 placeholder loot-table set (ITM-02). A small ORIGINAL, clean-room set
// (loot_table.cpp) keyed to kPlaceholderCreatureIdBase creatures and referencing
// the meridian::items placeholder item templates (kPlaceholderIdBase). Exercises
// every axis the roll/session code must handle: guaranteed money, a common weighted
// group, a rare drop, and a quest-gated drop. NOT the content pipeline — the seam's
// stand-in, dropped when mcc #28 lands.
class PlaceholderLootTableStore : public LootTableStore {
public:
    PlaceholderLootTableStore();
    const LootTable* find(std::uint32_t creature_template_id) const override;

    // Every placeholder creature id that carries loot, ascending (tests / tooling).
    std::vector<std::uint32_t> ids() const;

private:
    std::unordered_map<std::uint32_t, LootTable> by_creature_;
};

}  // namespace meridian::loot
