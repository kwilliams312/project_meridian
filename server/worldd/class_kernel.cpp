// SPDX-License-Identifier: Apache-2.0
//
// worldd — class kernel: equip-gating + category-match + role hooks (class_kernel.h).
// Clean-room, original code (CONTRIBUTING.md).

#include "class_kernel.h"

#include <algorithm>

namespace meridian::worldd {

// --- enum <-> token ----------------------------------------------------------

bool parse_combat_role(const std::string& s, CombatRole& out) {
    if (s == "healer") { out = CombatRole::kHealer; return true; }
    if (s == "dps_melee") { out = CombatRole::kDpsMelee; return true; }
    if (s == "dps_ranged") { out = CombatRole::kDpsRanged; return true; }
    if (s == "tank") { out = CombatRole::kTank; return true; }
    return false;
}

bool parse_equip_category(const std::string& s, EquipCategory& out) {
    if (s == "armor") { out = EquipCategory::kArmor; return true; }
    if (s == "weapon") { out = EquipCategory::kWeapon; return true; }
    return false;
}

const char* combat_role_name(CombatRole r) {
    switch (r) {
        case CombatRole::kHealer:    return "healer";
        case CombatRole::kDpsMelee:  return "dps_melee";
        case CombatRole::kDpsRanged: return "dps_ranged";
        case CombatRole::kTank:      return "tank";
    }
    return "?";
}

const char* equip_category_name(EquipCategory c) {
    switch (c) {
        case EquipCategory::kArmor:  return "armor";
        case EquipCategory::kWeapon: return "weapon";
    }
    return "?";
}

// --- EquipTypeCatalog --------------------------------------------------------

void EquipTypeCatalog::add(EquipTypeDef def) {
    const std::uint32_t id = def.content_id;
    if (id == 0) return;  // 0 is the reserved null id — never a real entity
    by_ref_[def.ref] = id;
    by_id_[id] = std::move(def);
}

const EquipTypeDef* EquipTypeCatalog::find(std::uint32_t content_id) const {
    const auto it = by_id_.find(content_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

const EquipTypeDef* EquipTypeCatalog::find_by_ref(const std::string& ref) const {
    const auto it = by_ref_.find(ref);
    return it == by_ref_.end() ? nullptr : find(it->second);
}

// --- ClassRecord / ClassCatalog ----------------------------------------------

bool ClassRecord::has_role(CombatRole r) const {
    return std::find(roles.begin(), roles.end(), r) != roles.end();
}

ClassRecord& ClassCatalog::touch(std::uint8_t roster_id) {
    ClassRecord& rec = by_id_[roster_id];
    rec.roster_id = roster_id;
    return rec;
}

const ClassRecord* ClassCatalog::find(std::uint8_t roster_id) const {
    const auto it = by_id_.find(roster_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

// --- slot family -------------------------------------------------------------

SlotFamily slot_family(items::ItemSlot slot) {
    switch (slot) {
        case items::ItemSlot::kHead:
        case items::ItemSlot::kShoulders:
        case items::ItemSlot::kBack:
        case items::ItemSlot::kChest:
        case items::ItemSlot::kWrist:
        case items::ItemSlot::kHands:
        case items::ItemSlot::kWaist:
        case items::ItemSlot::kLegs:
        case items::ItemSlot::kFeet:
            return SlotFamily::kArmor;
        case items::ItemSlot::kMainHand:
        case items::ItemSlot::kOffHand:
        case items::ItemSlot::kTwoHand:
        case items::ItemSlot::kRanged:
            return SlotFamily::kWeapon;
        case items::ItemSlot::kNeck:
        case items::ItemSlot::kFinger:
        case items::ItemSlot::kTrinket:
            return SlotFamily::kAccessory;
        case items::ItemSlot::kNone:
        case items::ItemSlot::kBag:
            return SlotFamily::kNone;
    }
    return SlotFamily::kNone;
}

// --- equip gate --------------------------------------------------------------

const char* equip_gate_reason(EquipGate g) {
    switch (g) {
        case EquipGate::kAllowed:          return "";
        case EquipGate::kNotProficient:    return "your class cannot use that equipment type";
        case EquipGate::kCategoryMismatch: return "that item's type does not match its slot";
        case EquipGate::kUnknownEquipType: return "that item has an unknown equipment type";
    }
    return "";
}

EquipGate gate_equip(const ClassRecord& cls, const EquipTypeCatalog& catalog,
                     const items::ItemTemplate& item) {
    const SlotFamily fam = slot_family(item.slot);

    // Accessories (jewellery) and non-equippable slots carry no armor/weapon
    // proficiency — never gated by class type.
    if (fam == SlotFamily::kAccessory || fam == SlotFamily::kNone) {
        return EquipGate::kAllowed;
    }

    // A weapon/armor item with no equip_type declared (legacy / back-compat — the
    // schema field is optional) carries no proficiency, so it cannot be gated.
    if (item.equip_type_id == 0) {
        return EquipGate::kAllowed;
    }

    const EquipTypeDef* et = catalog.find(item.equip_type_id);
    if (et == nullptr) {
        return EquipGate::kUnknownEquipType;  // malformed pack — ref did not resolve
    }

    // CATEGORY-MATCH (the closed SP1-deferred check): the item's equip_type category
    // must match its paperdoll slot family. An armor-slot equip of a weapon-category
    // item (or vice versa) is rejected here at runtime.
    const bool weapon_slot = (fam == SlotFamily::kWeapon);
    const bool weapon_type = (et->category == EquipCategory::kWeapon);
    if (weapon_slot != weapon_type) {
        return EquipGate::kCategoryMismatch;
    }

    // PROFICIENCY: the class's usable list for that category must contain the type.
    if (weapon_type) {
        return cls.can_use_weapon(item.equip_type_id) ? EquipGate::kAllowed
                                                       : EquipGate::kNotProficient;
    }
    return cls.can_use_armor(item.equip_type_id) ? EquipGate::kAllowed
                                                 : EquipGate::kNotProficient;
}

// --- role hook ---------------------------------------------------------------

float threat_multiplier(const ClassRecord& cls) {
    return cls.has_role(CombatRole::kTank) ? kTankThreatMultiplier : 1.0f;
}

}  // namespace meridian::worldd
