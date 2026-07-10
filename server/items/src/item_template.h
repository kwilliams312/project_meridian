// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — item TEMPLATES + the read-only template datastore seam
// (ITM-01; server PRD §4-M1 "item instances vs item templates"; issue #366).
//
// Provenance / clean-room basis (CONTRIBUTING.md — no GPL/AGPL/CMaNGOS/
// TrinityCore/leaked source consulted):
//   * The template FIELD SET mirrors OUR content schema (schema/content/
//     item.schema.yaml, `meridian/item@2`) and its compiled world-DB shape
//     (schema/sql/world/20_item.sql `item_template` / `item_stat`). Those two
//     files are the authoritative item model; this struct is the server-side
//     in-memory view of one compiled row.
//   * The TEMPLATE vs INSTANCE split is server PRD §4-M1 (ITM-01): a template is
//     a STATIC definition (id, name, slot, stackability, value, stat mods) shared
//     by every copy; an instance (inventory.h) is a concrete server-minted item
//     with a guid + stack + owner. Templates never change at runtime.
//
// DATASTORE SEAM (ITM-01, mcc #28): templates are a read-only artifact. At M1
// the real template pipeline (mcc #28 compiles item.schema.yaml -> the world DB
// `item_template` table, replaced wholesale nightly) is NOT built. This header
// therefore exposes an ABSTRACT `TemplateStore` (the seam) plus a
// `PlaceholderTemplateStore` (a small original M1 set, item_template.cpp). When
// mcc #28 lands, a `WorldDbTemplateStore` implements the SAME seam over the world
// DB and the placeholder set is dropped — no inventory/loot/vendor code changes.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace meridian::items {

// Whole-copper currency amount (ECO-01). SIGNED int64 per the PRD ("money as
// int64 copper") — the value is ALWAYS in [0, kMaxCopper] (never negative, never
// FLOAT). The durable column `character.money` is BIGINT UNSIGNED and can hold
// more than INT64_MAX, but the economy API caps balances at kMaxCopper so every
// value round-trips through a signed int64 exactly. See currency.h.
using Copper = std::int64_t;

// Item class — mirrors item.schema.yaml `item_class` / world DB
// item_template.item_class ENUM. Governs which validations apply (only weapon/
// armor are equippable; consumable/quest/trade_good live in the backpack).
enum class ItemClass : std::uint8_t {
    kWeapon = 1,
    kArmor,
    kConsumable,
    kQuest,
    kTradeGood,
    kContainer,
};

// The item's SLOT TYPE — mirrors item.schema.yaml `slot` / world DB
// item_template.slot ENUM, with kNone (0) for a non-equippable item (the schema
// leaves `slot` NULL). This is the item's property; the character's paperdoll
// POSITIONS are EquipSlot (below). A kTwoHand item occupies the MainHand paperdoll
// position and blocks the off hand (see inventory.h equip rules).
enum class ItemSlot : std::uint8_t {
    kNone = 0,  // not equippable (consumable/quest/trade good, or a bag stored loose)
    kHead,
    kShoulders,
    kBack,
    kChest,
    kWrist,
    kHands,
    kWaist,
    kLegs,
    kFeet,
    kNeck,
    kFinger,
    kTrinket,
    kMainHand,
    kOffHand,
    kTwoHand,
    kRanged,
    kBag,
};

// Rarity tier — mirrors item.schema.yaml `rarity`. Carried for loot/vendor/UI use
// (ITM-03 stat-budget enforcement is M2); does not affect M1 inventory validation.
enum class Rarity : std::uint8_t {
    kPoor = 0,
    kCommon,
    kUncommon,
    kRare,
    kEpic,
    kLegendary,
};

// Bind-on rule — mirrors item.schema.yaml `binding` / world DB
// item_template.binding. Carried for later trade/soulbind logic; inert in M1.
enum class Binding : std::uint8_t {
    kNone = 0,
    kOnPickup,
    kOnEquip,
};

// Primary-stat key — mirrors item.schema.yaml `$defs/statKey` / world DB
// item_stat.stat ENUM. The M1 stat set.
enum class StatKey : std::uint8_t {
    kStrength = 1,
    kAgility,
    kStamina,
    kIntellect,
    kSpirit,
};

// One stat modifier granted by an item (item_stat row). `amount` is SIGNED — the
// schema allows negative amounts (item_stat.amount INT, "can be negative").
struct StatMod {
    StatKey stat = StatKey::kStrength;
    std::int32_t amount = 0;
};

// Optional weapon block — mirrors item.schema.yaml `weapon` / world DB
// item_template.weapon_*. Present only when item_class == kWeapon. Not used by M1
// inventory/equip validation (combat is CMB-01); carried so the template model is
// faithful for the systems that consume it later.
struct WeaponData {
    std::uint32_t damage_min = 0;
    std::uint32_t damage_max = 0;
    std::uint32_t speed_ms = 0;
};

// A STATIC item definition (ITM-01). One per distinct item; shared by every
// instance of that item. Read-only at runtime — produced by the template
// datastore (placeholder set now, mcc #28 later). Field set mirrors
// item.schema.yaml / schema/sql/world/20_item.sql.
struct ItemTemplate {
    std::uint32_t id = 0;             // IF-9 numeric id (world DB item_template.id)
    std::string name;                 // display name (item_template.name)
    ItemClass item_class = ItemClass::kTradeGood;
    ItemSlot slot = ItemSlot::kNone;  // equip slot type; kNone = not equippable
    Rarity rarity = Rarity::kCommon;
    std::uint16_t required_level = 1;  // min character level to EQUIP (item_template.required_level)
    std::uint16_t item_level = 1;      // itemization tier (ITM-03/M2); inert in M1
    bool unique = false;               // one-per-character (item_template.is_unique); reserved for M2
    Binding binding = Binding::kNone;
    std::uint16_t max_stack = 1;       // stack_size; 1 == not stackable
    std::vector<StatMod> stats;        // item_stat rows
    std::uint32_t armor = 0;           // top-level armor value (0 = none)
    std::optional<WeaponData> weapon;  // present iff item_class == kWeapon
    std::optional<Copper> sell_price;  // vendor sell-back (nullopt = unsellable, per schema)
    std::optional<Copper> buy_price;   // default vendor purchase price (nullopt = not sold)

    // A stackable item is one whose stacks may hold more than one unit.
    bool is_stackable() const { return max_stack > 1; }

    // True iff the item can be worn in a paperdoll slot. Weapons/armor with a
    // concrete slot are equippable; a loose bag (kBag) is a container the player
    // opens, not gear, and is NOT equippable in M1 (bag>0 containers are M2).
    bool is_equippable() const {
        return slot != ItemSlot::kNone && slot != ItemSlot::kBag;
    }
};

// --- Template datastore seam (ITM-01, mcc #28) -------------------------------
// The read-only source of templates. Inventory/loot/vendor code depends ONLY on
// this interface, never on where templates come from. M1 wires the placeholder
// implementation below; mcc #28 later adds a world-DB implementation of the SAME
// interface and the placeholder set is deleted — no consumer changes.
class TemplateStore {
public:
    virtual ~TemplateStore() = default;

    // Return the template for `template_id`, or nullptr if unknown. The returned
    // pointer is owned by the store and is valid for the store's lifetime.
    virtual const ItemTemplate* find(std::uint32_t template_id) const = 0;
};

// Reserved id range for the M1 PLACEHOLDER template set. Real content ids (mcc
// #28) are authored in content and will not collide with this dev-only range —
// keeping placeholder ids distinct means a stray placeholder never masquerades as
// a real compiled template once #28 lands.
inline constexpr std::uint32_t kPlaceholderIdBase = 900000;

// The M1 placeholder template set (ITM-01). A small ORIGINAL, clean-room set of
// generic archetypes (see item_template.cpp) — just enough for loot (#369),
// vendors (#370) and equip validation to have real data to operate on before mcc
// #28 produces the content-authored templates. NOT the content pipeline: it is
// the seam's stand-in implementation, dropped when #28 lands.
class PlaceholderTemplateStore : public TemplateStore {
public:
    PlaceholderTemplateStore();
    const ItemTemplate* find(std::uint32_t template_id) const override;

    // Every placeholder id, ascending (tests / dev tooling).
    std::vector<std::uint32_t> ids() const;

private:
    std::unordered_map<std::uint32_t, ItemTemplate> by_id_;
};

}  // namespace meridian::items
