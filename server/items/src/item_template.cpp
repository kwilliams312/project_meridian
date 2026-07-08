// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — the M1 PLACEHOLDER template set (ITM-01; issue #366).
// See item_template.h for the datastore-seam rationale (mcc #28 replaces this).
//
// These templates are ORIGINAL, clean-room placeholders (CONTRIBUTING.md — no
// GPL/AGPL/CMaNGOS/TrinityCore/leaked roster consulted). They are deliberately
// generic low-level archetypes themed to the M1 roster (roster.h: Ardent race,
// Vanguard melee + Runcaller caster) and cover every axis the inventory/equip/
// vendor/loot code must exercise: a one-hand weapon, a two-hand weapon, a shield
// (off hand), armor in several paperdoll slots, a level-gated ring, stackable
// consumables and stackable trade goods, and an unsellable quest item. Field
// values mirror the shapes in schema/content/item.schema.yaml.

#include "item_template.h"

namespace meridian::items {

namespace {

// Build one placeholder template. Kept terse so the set below reads as a table.
ItemTemplate make(std::uint32_t id, const char* name, ItemClass cls, ItemSlot slot,
                  Rarity rarity, std::uint16_t req_level, std::uint16_t max_stack,
                  std::optional<Copper> sell, std::optional<Copper> buy,
                  std::vector<StatMod> stats = {}, std::uint32_t armor = 0,
                  std::optional<WeaponData> weapon = std::nullopt) {
    ItemTemplate t;
    t.id = id;
    t.name = name;
    t.item_class = cls;
    t.slot = slot;
    t.rarity = rarity;
    t.required_level = req_level;
    t.max_stack = max_stack;
    t.sell_price = sell;
    t.buy_price = buy;
    t.stats = std::move(stats);
    t.armor = armor;
    t.weapon = weapon;
    return t;
}

}  // namespace

PlaceholderTemplateStore::PlaceholderTemplateStore() {
    constexpr std::uint32_t B = kPlaceholderIdBase;

    const ItemTemplate set[] = {
        // id, name, class, slot, rarity, reqLvl, maxStack, sell, buy, stats, armor, weapon
        make(B + 1, "Worn Shortsword", ItemClass::kWeapon, ItemSlot::kMainHand,
             Rarity::kCommon, 1, 1, Copper{25}, Copper{100},
             {{StatKey::kStrength, 1}}, 0, WeaponData{2, 4, 2200}),
        make(B + 2, "Cracked Buckler", ItemClass::kArmor, ItemSlot::kOffHand,
             Rarity::kCommon, 1, 1, Copper{18}, Copper{75}, {}, 20),
        make(B + 3, "Apprentice Staff", ItemClass::kWeapon, ItemSlot::kTwoHand,
             Rarity::kCommon, 1, 1, Copper{40}, Copper{160},
             {{StatKey::kIntellect, 2}}, 0, WeaponData{3, 6, 3000}),
        make(B + 4, "Rugged Leather Vest", ItemClass::kArmor, ItemSlot::kChest,
             Rarity::kUncommon, 1, 1, Copper{30}, Copper{120},
             {{StatKey::kStamina, 1}}, 30),
        make(B + 5, "Simple Cloth Cap", ItemClass::kArmor, ItemSlot::kHead,
             Rarity::kCommon, 1, 1, Copper{8}, Copper{35},
             {{StatKey::kSpirit, 1}}, 5),
        make(B + 6, "Traveler's Ring", ItemClass::kArmor, ItemSlot::kFinger,
             Rarity::kUncommon, 5, 1, Copper{50}, Copper{200},
             {{StatKey::kAgility, 1}}),
        make(B + 7, "Minor Health Potion", ItemClass::kConsumable, ItemSlot::kNone,
             Rarity::kCommon, 1, 20, Copper{1}, Copper{5}),
        make(B + 8, "Copper Ore", ItemClass::kTradeGood, ItemSlot::kNone,
             Rarity::kCommon, 1, 100, Copper{2}, std::nullopt),
        // Quest item: unsellable (no sell_price) and not stackable.
        make(B + 9, "Tattered Dispatch", ItemClass::kQuest, ItemSlot::kNone,
             Rarity::kPoor, 1, 1, std::nullopt, std::nullopt),
    };

    for (const ItemTemplate& t : set) {
        by_id_.emplace(t.id, t);
    }
}

const ItemTemplate* PlaceholderTemplateStore::find(std::uint32_t template_id) const {
    auto it = by_id_.find(template_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> PlaceholderTemplateStore::ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(by_id_.size());
    for (const auto& [id, _] : by_id_) out.push_back(id);
    // Ascending so callers/tests get a deterministic order (map is unordered).
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (out[j] < out[i]) std::swap(out[i], out[j]);
        }
    }
    return out;
}

}  // namespace meridian::items
