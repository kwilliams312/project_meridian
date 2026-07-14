// SPDX-License-Identifier: Apache-2.0
//
// worldd — class kernel unit test (SP2.7 #697, epic #690): equip-gating +
// category-match + role hook + talent application, driven over in-memory catalogs
// (no DB). The DB-backed proof lives in equip_gating_db_it.cpp; this locks the pure
// decision logic. Always runs in the plain server ctest (no MERIDIAN_DB_* needed).
//
// Clean-room, original code (CONTRIBUTING.md).

#include "class_kernel.h"
#include "talent_catalog.h"

#include <cstdio>

using namespace meridian::worldd;
namespace itm = meridian::items;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Numeric equip_type ids mirroring the seed pack emit (see world.sql):
// mail=120, one_hand=121, plate=122, staff=123, two_hand=124, leather=119.
constexpr std::uint32_t kMail = 120, kOneHand = 121, kPlate = 122, kStaff = 123,
                        kTwoHand = 124, kLeather = 119;

EquipTypeCatalog make_catalog() {
    EquipTypeCatalog c;
    c.add({kLeather, "core:equip_type.leather", "Leather", EquipCategory::kArmor, ""});
    c.add({kMail, "core:equip_type.mail", "Mail", EquipCategory::kArmor, ""});
    c.add({kPlate, "core:equip_type.plate", "Plate", EquipCategory::kArmor, ""});
    c.add({kOneHand, "core:equip_type.one_hand", "One-Hand", EquipCategory::kWeapon, "main"});
    c.add({kStaff, "core:equip_type.staff", "Staff", EquipCategory::kWeapon, "two_hand"});
    c.add({kTwoHand, "core:equip_type.two_hand", "Two-Hand", EquipCategory::kWeapon, "two_hand"});
    return c;
}

// The seed Vanguard (roster 1): armor {plate, mail}, weapon {one_hand, two_hand},
// role tank, talent tree 143.
ClassRecord vanguard() {
    ClassRecord r;
    r.roster_id = 1;
    r.name = "Vanguard";
    r.usable_armor_types = {kPlate, kMail};
    r.usable_weapon_types = {kOneHand, kTwoHand};
    r.roles = {CombatRole::kTank};
    r.talent_tree_id = 143;
    return r;
}

// The seed Warden (roster 3): armor {leather, mail}, weapon {one_hand, staff},
// hybrid {dps_ranged, healer}.
ClassRecord warden() {
    ClassRecord r;
    r.roster_id = 3;
    r.name = "Warden";
    r.usable_armor_types = {kLeather, kMail};
    r.usable_weapon_types = {kOneHand, kStaff};
    r.roles = {CombatRole::kDpsRanged, CombatRole::kHealer};
    return r;
}

itm::ItemTemplate item(itm::ItemSlot slot, std::uint32_t equip_type_id,
                       itm::ItemClass cls = itm::ItemClass::kArmor) {
    itm::ItemTemplate t;
    t.id = 1;
    t.slot = slot;
    t.equip_type_id = equip_type_id;
    t.item_class = cls;
    return t;
}

}  // namespace

int main() {
    std::printf("worldd class-kernel unit test (#697)\n");

    const EquipTypeCatalog cat = make_catalog();
    const ClassRecord vg = vanguard();
    const ClassRecord wd = warden();

    // ---- slot family classification ---------------------------------------
    check("chest is an armor slot", slot_family(itm::ItemSlot::kChest) == SlotFamily::kArmor);
    check("main_hand is a weapon slot", slot_family(itm::ItemSlot::kMainHand) == SlotFamily::kWeapon);
    check("two_hand is a weapon slot", slot_family(itm::ItemSlot::kTwoHand) == SlotFamily::kWeapon);
    check("finger is an accessory slot", slot_family(itm::ItemSlot::kFinger) == SlotFamily::kAccessory);
    check("none is not a slot", slot_family(itm::ItemSlot::kNone) == SlotFamily::kNone);

    // ---- equip-gating: ACCEPT ---------------------------------------------
    // Vanguard equips a plate chest (plate ∈ armor list, category matches slot).
    check("vanguard equips plate chest -> allowed",
          gate_equip(vg, cat, item(itm::ItemSlot::kChest, kPlate)) == EquipGate::kAllowed);
    // Vanguard equips a one-hand weapon (one_hand ∈ weapon list).
    check("vanguard equips one-hand weapon -> allowed",
          gate_equip(vg, cat, item(itm::ItemSlot::kMainHand, kOneHand, itm::ItemClass::kWeapon))
              == EquipGate::kAllowed);

    // ---- equip-gating: REJECT (class cannot use the type) -----------------
    // Warden cannot equip plate (plate ∉ {leather, mail}).
    check("warden equips plate chest -> not proficient",
          gate_equip(wd, cat, item(itm::ItemSlot::kChest, kPlate)) == EquipGate::kNotProficient);
    // Warden cannot wield a two-hand (∉ {one_hand, staff}).
    check("warden wields two-hand -> not proficient",
          gate_equip(wd, cat, item(itm::ItemSlot::kTwoHand, kTwoHand, itm::ItemClass::kWeapon))
              == EquipGate::kNotProficient);

    // ---- category-match: REJECT (armor slot <- weapon-category item) ------
    // A weapon-category equip_type in an ARMOR slot is rejected (the closed
    // SP1-deferred check), regardless of class proficiency.
    check("weapon-type item in a chest slot -> category mismatch",
          gate_equip(vg, cat, item(itm::ItemSlot::kChest, kOneHand)) == EquipGate::kCategoryMismatch);
    // And the reverse: an armor-category type in a weapon slot.
    check("armor-type item in a main-hand slot -> category mismatch",
          gate_equip(vg, cat, item(itm::ItemSlot::kMainHand, kPlate, itm::ItemClass::kWeapon))
              == EquipGate::kCategoryMismatch);

    // ---- ungated cases -----------------------------------------------------
    check("accessory (finger) is ungated",
          gate_equip(wd, cat, item(itm::ItemSlot::kFinger, 0)) == EquipGate::kAllowed);
    check("armor item with no equip_type is ungated (back-compat)",
          gate_equip(wd, cat, item(itm::ItemSlot::kChest, 0)) == EquipGate::kAllowed);
    check("unknown equip_type id -> rejected",
          gate_equip(vg, cat, item(itm::ItemSlot::kChest, 99999)) == EquipGate::kUnknownEquipType);

    // ---- role hook ---------------------------------------------------------
    check("tank class amplifies threat (2.0x)", threat_multiplier(vg) == kTankThreatMultiplier);
    check("non-tank class threat is neutral (1.0x)", threat_multiplier(wd) == 1.0f);
    check("vanguard has the tank role", vg.has_role(CombatRole::kTank));
    check("warden has no tank role", !wd.has_role(CombatRole::kTank));

    // ---- talent application ------------------------------------------------
    // Build the seed vanguard_path tree: tier0 (0 pts) -> battle_fury (141:
    // grants ability 133 + strength +5 flat); tier1 (5 pts) -> warding_grace
    // (142: grants ability 135 + intellect +3 flat).
    TalentCatalog tc;
    {
        TalentDef& bf = tc.touch_talent(141);
        bf.grants.push_back({TalentGrant::Kind::kAbility, 133, "", 0, AttributeModifier::kFlat, 0, 1});
        bf.grants.push_back({TalentGrant::Kind::kBuff, 0, "core:attribute.strength", 5,
                             AttributeModifier::kFlat, 0, 1});
        TalentDef& wg = tc.touch_talent(142);
        wg.grants.push_back({TalentGrant::Kind::kAbility, 135, "", 0, AttributeModifier::kFlat, 0, 1});
        wg.grants.push_back({TalentGrant::Kind::kBuff, 0, "core:attribute.intellect", 3,
                             AttributeModifier::kFlat, 0, 1});
        TalentTreeDef& tree = tc.touch_tree(143);
        tree.tiers.push_back({0, {141}});
        tree.tiers.push_back({5, {142}});
    }
    const TalentTreeDef* tree = tc.find_tree(143);
    check("tree 143 loaded", tree != nullptr);

    // 0 points: only tier 0 unlocked -> battle_fury.
    const TalentApplication a0 = apply_talents(*tree, tc, /*points=*/0);
    check("0 pts: grants ability 133 only",
          a0.granted_ability_ids.size() == 1 && a0.granted_ability_ids[0] == 133);
    check("0 pts: strength passive +5", a0.passive("core:attribute.strength").flat == 5);
    check("0 pts: intellect passive absent", a0.passive("core:attribute.intellect").flat == 0);

    // 5 points: both tiers unlocked -> both talents.
    const TalentApplication a5 = apply_talents(*tree, tc, /*points=*/5);
    check("5 pts: grants abilities 133 + 135",
          a5.granted_ability_ids.size() == 2 && a5.granted_ability_ids[0] == 133 &&
              a5.granted_ability_ids[1] == 135);
    check("5 pts: strength +5 AND intellect +3",
          a5.passive("core:attribute.strength").flat == 5 &&
              a5.passive("core:attribute.intellect").flat == 3);

    if (g_fail == 0) {
        std::printf("PASS: all class-kernel checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d class-kernel check(s) failed\n", g_fail);
    return 1;
}
