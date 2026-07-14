// SPDX-License-Identifier: Apache-2.0
//
// worldd — the class KERNEL: equip-gating + category-match + role hooks (SP2.7
// #697, epic #690; SP2 design §2.4). This is the server-authoritative policy layer
// that turns SP1's rules-data catalogs (equip_type categories, per-class usable
// armor/weapon types, class roles) into live gameplay rules the kernel enforces.
//
// WHAT THIS FILE IS:
//   * EquipTypeCatalog — the loaded armor/weapon TYPE vocabulary (equip_type rows),
//     mapping an equip_type's IF-9 numeric id -> its category (armor | weapon). This
//     is the cross-entity lookup the STATIC content validators lacked (spec §2.4):
//     with it, the kernel can enforce the SP1-DEFERRED category-match at runtime.
//   * ClassRecord / ClassCatalog — the per-class rules the roster identity (SP2.5)
//     did not carry: the set of usable armor equip_types, the set of usable weapon
//     equip_types, the class's combat role(s), and its talent tree id.
//   * gate_equip() — the equip-time decision: an item may be equipped by a class iff
//     (a) its equip_type's CATEGORY matches its paperdoll SLOT FAMILY (armor-slot
//     item must be an armor-category type; weapon-slot must be weapon-category — the
//     closed SP1-deferred check), AND (b) the class's usable list for that category
//     contains the item's equip_type. Accessories (neck/finger/trinket) and items
//     with no equip_type are ungated (allowed).
//   * threat_multiplier() — the ROLE hook: a Tank-role class generates amplified
//     threat (the map-tick resolver->AI threat seam multiplies by this).
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE: plain deterministic data + arithmetic,
// like effective_stats + combat_resolver. The catalogs are filled once at boot from
// the world DB (db_content_store::load_db_class_catalog / load_db_equip_types); the
// logic here touches no DB, socket, or clock and runs in the plain `server` ctest.
//
// CLEAN-ROOM: designed from SP2 design §2.4, the pack-contract content schemas
// (equip_type / class), and the existing item model (item_template.h). Every rule
// is ORIGINAL — no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted. See
// CONTRIBUTING.md.

#ifndef MERIDIAN_WORLDD_CLASS_KERNEL_H
#define MERIDIAN_WORLDD_CLASS_KERNEL_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "item_template.h"  // meridian::items::ItemTemplate / ItemSlot

namespace meridian::worldd {

// A combat role — mirrors class.schema.yaml `role` / `hybrid` ENUM and the world
// DDL class_role.role ENUM. A class carries one role (single-role) or several
// (hybrid); the kernel reads the set to drive role hooks.
enum class CombatRole : std::uint8_t {
    kHealer,
    kDpsMelee,
    kDpsRanged,
    kTank,
};

// An equip-type category — mirrors equip_type.schema.yaml `category` / the world
// DDL equip_type.category ENUM. armor = a wearable material class (Cloth/…/Plate);
// weapon = a weapon type (One-Hand/Two-Hand/Staff/Wand).
enum class EquipCategory : std::uint8_t {
    kArmor,
    kWeapon,
};

// Parse a role / category token (DB ENUM string). Returns false on an unknown
// token (a malformed pack row — the caller degrades rather than throwing).
bool parse_combat_role(const std::string& s, CombatRole& out);
bool parse_equip_category(const std::string& s, EquipCategory& out);
const char* combat_role_name(CombatRole r);
const char* equip_category_name(EquipCategory c);

// One loaded equip_type (armor or weapon type), keyed by its IF-9 numeric id.
struct EquipTypeDef {
    std::uint32_t content_id = 0;                 // IF-9 numeric id
    std::string   ref;                            // verbatim contentId (core:equip_type.plate)
    std::string   name;                           // displayName
    EquipCategory category = EquipCategory::kArmor;
    std::string   slot_class;                     // informational grouping (may be empty)
};

// ---------------------------------------------------------------------------
// EquipTypeCatalog — the loaded armor/weapon type vocabulary, read-only after boot.
// ---------------------------------------------------------------------------
class EquipTypeCatalog {
public:
    EquipTypeCatalog() = default;

    // Register one equip_type (idempotent replace by content_id).
    void add(EquipTypeDef def);

    // The definition for a numeric id / a ref, or nullptr when unknown.
    const EquipTypeDef* find(std::uint32_t content_id) const;
    const EquipTypeDef* find_by_ref(const std::string& ref) const;

    std::size_t size() const { return by_id_.size(); }

private:
    std::unordered_map<std::uint32_t, EquipTypeDef> by_id_;
    std::unordered_map<std::string, std::uint32_t>  by_ref_;  // ref -> content_id
};

// ---------------------------------------------------------------------------
// ClassRecord — one class's equip-gating + role/talent rules (beyond the SP2.5
// identity). Keyed by the canonical roster_id (character.class), matching the
// runtime Roster + the attribute mods, so a character's class id looks its record
// up directly.
// ---------------------------------------------------------------------------
struct ClassRecord {
    std::uint8_t roster_id = 0;
    std::string  name;
    // The equip_type numeric ids the class may equip, split by authoring list. A
    // set so membership is a direct lookup and the dump order is deterministic.
    std::set<std::uint32_t> usable_armor_types;
    std::set<std::uint32_t> usable_weapon_types;
    // The class's role(s) — one for a single-role class, 2-4 for a hybrid.
    std::vector<CombatRole> roles;
    // The class's talent tree (talent_tree numeric id), or 0 when it has none.
    std::uint32_t talent_tree_id = 0;

    bool can_use_armor(std::uint32_t equip_type_id) const {
        return usable_armor_types.count(equip_type_id) != 0;
    }
    bool can_use_weapon(std::uint32_t equip_type_id) const {
        return usable_weapon_types.count(equip_type_id) != 0;
    }
    bool has_role(CombatRole r) const;
};

// ---------------------------------------------------------------------------
// ClassCatalog — every loaded ClassRecord, keyed by roster_id.
// ---------------------------------------------------------------------------
class ClassCatalog {
public:
    ClassCatalog() = default;

    // Get-or-create the record for `roster_id` (the loaders build it incrementally
    // from the identity row + the child usable-type / role rows).
    ClassRecord& touch(std::uint8_t roster_id);
    const ClassRecord* find(std::uint8_t roster_id) const;

    std::size_t size() const { return by_id_.size(); }

private:
    std::map<std::uint8_t, ClassRecord> by_id_;
};

// The slot FAMILY of an item's paperdoll slot — what governs which proficiency
// list (if any) gates it, and which equip_type category it must carry.
enum class SlotFamily : std::uint8_t {
    kArmor,      // head/shoulders/back/chest/wrist/hands/waist/legs/feet
    kWeapon,     // main_hand/off_hand/two_hand/ranged
    kAccessory,  // neck/finger/trinket — NOT gated by armor/weapon proficiency
    kNone,       // not equippable (bag / kNone)
};
SlotFamily slot_family(items::ItemSlot slot);

// The outcome of an equip-gate decision (SP2.7 #697).
enum class EquipGate : std::uint8_t {
    kAllowed,           // the class may equip this item
    kNotProficient,     // the class's usable list for the item's category lacks its equip_type
    kCategoryMismatch,  // the item's equip_type category does not match its slot family
    kUnknownEquipType,  // the item names an equip_type not in the catalog (malformed pack)
};

// Human-readable one-line reason for a gate outcome (for logs / a reject status /
// test diagnostics). "" for kAllowed.
const char* equip_gate_reason(EquipGate g);

// The equip-time decision: may a character of class `cls` equip `item`?
//   1. Accessory / non-equippable slots, and items with NO equip_type, are ungated
//      (kAllowed) — jewellery and legacy items carry no proficiency.
//   2. The item's equip_type must resolve in `catalog` (else kUnknownEquipType).
//   3. CATEGORY-MATCH (the closed SP1-deferred check): an armor-slot item must carry
//      an armor-category equip_type; a weapon-slot item a weapon-category one — else
//      kCategoryMismatch.
//   4. PROFICIENCY: the class's usable_armor_types (armor) / usable_weapon_types
//      (weapon) must contain the item's equip_type — else kNotProficient.
EquipGate gate_equip(const ClassRecord& cls, const EquipTypeCatalog& catalog,
                     const items::ItemTemplate& item);

// The threat multiplier a class of role set generates (the ROLE hook, SP2.7 #697).
// A Tank-role class amplifies the threat it deals so it can hold aggro; every other
// role is neutral (1.0). The map-tick resolver->AI threat seam multiplies the raw
// threat by this before add_threat.
inline constexpr float kTankThreatMultiplier = 2.0f;
float threat_multiplier(const ClassRecord& cls);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_CLASS_KERNEL_H
