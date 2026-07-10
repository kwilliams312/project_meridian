// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — the M1 PLACEHOLDER loot-table set (ITM-02; issue #369).
// See loot_table.h for the datastore-seam rationale (mcc #28 replaces this).
//
// These tables are ORIGINAL, clean-room placeholders (CONTRIBUTING.md — no
// GPL/AGPL/CMaNGOS/TrinityCore/leaked loot tables consulted). They reference the
// meridian::items placeholder item templates (item_template.cpp,
// kPlaceholderIdBase) and are themed to the M1 roster. Together they cover every
// axis the roll/session code must exercise:
//   * guaranteed money (a range),
//   * a COMMON weighted group where exactly one of several items is chosen,
//   * a RARE low-chance single drop,
//   * a QUEST-gated drop conditioned on quest state (ITM-02 acceptance).

#include "loot_table.h"

#include "item_template.h"  // meridian::items::kPlaceholderIdBase

namespace meridian::loot {

namespace {

// Item ids from the meridian::items placeholder set (item_template.cpp).
constexpr std::uint32_t I = items::kPlaceholderIdBase;
constexpr std::uint32_t kMinorHealthPotion = I + 7;   // stackable consumable
constexpr std::uint32_t kCopperOre = I + 8;           // stackable trade good
constexpr std::uint32_t kWornShortsword = I + 1;      // a weapon (rare drop)
constexpr std::uint32_t kTatteredDispatch = I + 9;    // the quest item

}  // namespace

PlaceholderLootTableStore::PlaceholderLootTableStore() {
    // --- kCreatureWolf: a common mob -----------------------------------------
    // Guaranteed a few copper; usually drops one common trade item (potion far
    // more likely than ore); rarely drops a weapon.
    {
        LootTable t;
        t.money_min = 5;
        t.money_max = 20;
        // Common group: 80% chance to drop ONE of {potion (weight 3), ore
        // (weight 1)} — potion 3× as likely as ore.
        t.groups.push_back(LootGroup{
            /*chance_bp=*/8000,
            {
                LootEntry{kMinorHealthPotion, /*min*/ 1, /*max*/ 3, /*weight*/ 3, /*quest*/ 0},
                LootEntry{kCopperOre, /*min*/ 1, /*max*/ 2, /*weight*/ 1, /*quest*/ 0},
            }});
        // Rare group: 5% chance to drop a single Worn Shortsword.
        t.groups.push_back(LootGroup{
            /*chance_bp=*/500,
            {LootEntry{kWornShortsword, 1, 1, 1, 0}}});
        by_creature_.emplace(kCreatureWolf, std::move(t));
    }

    // --- kCreatureCourier: carries the quest item ----------------------------
    // Guaranteed money, a guaranteed common ore drop, and — ONLY for a player on
    // kPlaceholderQuestId — a guaranteed quest item (the loot SESSION gates the
    // quest drop per looter; the roll always produces it).
    {
        LootTable t;
        t.money_min = 10;
        t.money_max = 30;
        t.groups.push_back(LootGroup{
            /*chance_bp=*/kLootRollScale,  // always
            {LootEntry{kCopperOre, 1, 3, 1, 0}}});
        t.groups.push_back(LootGroup{
            /*chance_bp=*/kLootRollScale,  // always rolled; SESSION gates by quest
            {LootEntry{kTatteredDispatch, 1, 1, 1, kPlaceholderQuestId}}});
        by_creature_.emplace(kCreatureCourier, std::move(t));
    }
}

const LootTable* PlaceholderLootTableStore::find(std::uint32_t creature_template_id) const {
    auto it = by_creature_.find(creature_template_id);
    return it == by_creature_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> PlaceholderLootTableStore::ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(by_creature_.size());
    for (const auto& [id, _] : by_creature_) out.push_back(id);
    // Ascending so callers/tests get a deterministic order (map is unordered).
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        for (std::size_t j = i + 1; j < out.size(); ++j)
            if (out[j] < out[i]) std::swap(out[i], out[j]);
    return out;
}

}  // namespace meridian::loot
