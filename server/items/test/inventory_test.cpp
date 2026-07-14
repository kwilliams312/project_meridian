// SPDX-License-Identifier: Apache-2.0
//
// meridian-items DB-FREE unit test (ITM-01 + ECO-01; issue #366).
//
// Pure, deterministic domain logic — no MariaDB, no RNG, runs in every ctest.
// Proves the invariants the PRD calls out for M1:
//   * template datastore seam: placeholder set lookups + fields.
//   * inventory container: add / remove / stack-merge / split / merge / move.
//   * equip validation + slot rules: slot match, required level, two-hand vs
//     off-hand exclusivity, swap-back, unequip.
//   * int64 copper currency: checked add/subtract with NO underflow / overflow /
//     negative amounts.
//
// Clean-room, original code (CONTRIBUTING.md).

#include <cstdint>
#include <cstdio>

#include "currency.h"
#include "inventory.h"
#include "item_template.h"

using namespace meridian;
using namespace meridian::items;

namespace {

int g_fail = 0;

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Run `fn` and report whether it threw exception type E (and nothing else).
template <typename E, typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// Placeholder ids (item_template.cpp), named for readability.
constexpr std::uint32_t kSword = kPlaceholderIdBase + 1;    // main_hand weapon, req 1
constexpr std::uint32_t kBuckler = kPlaceholderIdBase + 2;  // off_hand armor
constexpr std::uint32_t kStaff = kPlaceholderIdBase + 3;    // two_hand weapon
constexpr std::uint32_t kVest = kPlaceholderIdBase + 4;     // chest armor
constexpr std::uint32_t kRing = kPlaceholderIdBase + 6;     // finger armor, req 5
constexpr std::uint32_t kPotion = kPlaceholderIdBase + 7;   // consumable, max_stack 20
constexpr std::uint32_t kOre = kPlaceholderIdBase + 8;      // trade good, max_stack 100

// Build an instance with a deterministic fake guid.
ItemInstance make_inst(std::uint32_t template_id, std::uint32_t stack,
                       std::uint64_t guid) {
    ItemInstance i;
    i.template_id = template_id;
    i.stack = stack;
    i.item_guid = guid;
    return i;
}

// ---- template datastore seam ------------------------------------------------
void test_templates() {
    std::printf("[templates]\n");
    PlaceholderTemplateStore store;

    check("placeholder set has 9 templates", store.ids().size() == 9);

    const ItemTemplate* sword = store.find(kSword);
    check("find(sword) returns a template", sword != nullptr);
    if (sword) {
        check("sword is a weapon", sword->item_class == ItemClass::kWeapon);
        check("sword equips in the main hand", sword->slot == ItemSlot::kMainHand);
        check("sword is equippable", sword->is_equippable());
        check("sword is not stackable", !sword->is_stackable());
        check("sword has a sell price", sword->sell_price.has_value());
    }

    const ItemTemplate* potion = store.find(kPotion);
    check("find(potion) returns a template", potion != nullptr);
    if (potion) {
        check("potion is stackable (max_stack 20)",
              potion->is_stackable() && potion->max_stack == 20);
        check("potion is NOT equippable", !potion->is_equippable());
    }

    check("find(unknown id) returns nullptr", store.find(123456) == nullptr);

    // Slot-type -> paperdoll mapping (equip_slot_for).
    check("two-hand maps to the main-hand paperdoll slot",
          equip_slot_for(ItemSlot::kTwoHand) == EquipSlot::kMainHand);
    check("a non-equippable slot maps to nullopt",
          !equip_slot_for(ItemSlot::kNone).has_value());
}

// ---- currency (pure) --------------------------------------------------------
void test_currency() {
    std::printf("[currency]\n");

    check("add 100 + 250 == 350", checked_add(100, 250) == 350);
    check("add 0 + 0 == 0", checked_add(0, 0) == 0);
    check("subtract 500 - 200 == 300", checked_subtract(500, 200) == 300);
    check("subtract to exactly zero is allowed", checked_subtract(200, 200) == 0);

    // No underflow: spending more than the balance is refused, not wrapped.
    check("subtract more than balance throws InsufficientFunds",
          throws<InsufficientFunds>([] { checked_subtract(100, 101); }));

    // No overflow: adding past the ceiling is refused.
    check("add past kMaxCopper throws CurrencyOverflow",
          throws<CurrencyOverflow>([] { checked_add(kMaxCopper, 1); }));
    check("add kMaxCopper to 0 is fine (no overflow at the boundary)",
          checked_add(0, kMaxCopper) == kMaxCopper);

    // A negative amount is a caller bug in either direction (explicit API).
    check("add with a negative amount throws NegativeAmount",
          throws<NegativeAmount>([] { checked_add(100, -1); }));
    check("subtract with a negative amount throws NegativeAmount",
          throws<NegativeAmount>([] { checked_subtract(100, -1); }));
}

// ---- inventory: add / remove ------------------------------------------------
void test_add_remove() {
    std::printf("[inventory add/remove]\n");
    PlaceholderTemplateStore store;
    Inventory inv(store, /*backpack_capacity=*/4);

    check("new backpack is empty", inv.backpack_used() == 0 && inv.backpack_free() == 4);

    inv.add(make_inst(kSword, 1, 10));
    inv.add(make_inst(kVest, 1, 11));
    check("two distinct items occupy two slots", inv.backpack_used() == 2);
    check("first free slot holds the sword",
          inv.backpack_at(0) && inv.backpack_at(0)->template_id == kSword);

    // Unknown template / bad stack counts are rejected, nothing added.
    check("add unknown template throws UnknownTemplate",
          throws<UnknownTemplate>([&] { inv.add(make_inst(999999, 1, 12)); }));
    check("add stack 0 throws BadStackCount",
          throws<BadStackCount>([&] { inv.add(make_inst(kPotion, 0, 12)); }));
    check("add stack > max_stack throws BadStackCount",
          throws<BadStackCount>([&] { inv.add(make_inst(kPotion, 21, 12)); }));
    check("rejected adds left the used count unchanged", inv.backpack_used() == 2);

    // Remove: partial decrements, full clears the slot.
    inv.add(make_inst(kPotion, 10, 20));  // slot 2
    Inventory::Removed r1 = inv.remove_from_backpack(2, 4);
    check("partial remove reports 4 removed", r1.count == 4 && !r1.slot_emptied);
    check("partial remove leaves stack 6",
          inv.backpack_at(2) && inv.backpack_at(2)->stack == 6);

    Inventory::Removed r2 = inv.remove_from_backpack(2, 6);
    check("removing the rest empties the slot", r2.slot_emptied);
    check("emptied slot is now free", inv.backpack_at(2) == nullptr);

    check("remove from an empty slot throws SlotEmpty",
          throws<SlotEmpty>([&] { inv.remove_from_backpack(2, 1); }));
    check("remove more than the stack throws BadStackCount",
          throws<BadStackCount>([&] { inv.remove_from_backpack(0, 5); }));
    check("out-of-range index throws InvalidSlot",
          throws<InvalidSlot>([&] { inv.remove_from_backpack(99, 1); }));
}

// ---- inventory: stack merge on add + capacity -------------------------------
void test_stacking() {
    std::printf("[inventory stacking]\n");
    PlaceholderTemplateStore store;
    Inventory inv(store, /*backpack_capacity=*/2);

    // A partial stack, then an add of the same template tops it up and spills the
    // remainder into the next slot (max_stack for ore is 100).
    inv.add(make_inst(kOre, 90, 30));
    inv.add(make_inst(kOre, 20, 31));  // 10 tops up slot0 -> 100, 10 spills to slot1
    check("first stack topped up to max_stack",
          inv.backpack_at(0) && inv.backpack_at(0)->stack == 100);
    check("remainder spilled into a second slot",
          inv.backpack_at(1) && inv.backpack_at(1)->stack == 10);

    // Backpack now: [100, 10], both slots used. Another 100 cannot fit (only 90
    // headroom in slot1) -> all-or-nothing InventoryFull, state unchanged.
    check("over-capacity add throws InventoryFull",
          throws<InventoryFull>([&] { inv.add(make_inst(kOre, 100, 32)); }));
    check("failed add did not mutate slot0", inv.backpack_at(0)->stack == 100);
    check("failed add did not mutate slot1", inv.backpack_at(1)->stack == 10);

    // Exactly the remaining headroom (90) fits into slot1.
    inv.add(make_inst(kOre, 90, 33));
    check("exact-headroom add fills slot1 to max_stack",
          inv.backpack_at(1)->stack == 100);
}

// ---- inventory: split / merge / move ----------------------------------------
void test_split_merge_move() {
    std::printf("[inventory split/merge/move]\n");
    PlaceholderTemplateStore store;
    Inventory inv(store, /*backpack_capacity=*/4);

    inv.add(make_inst(kOre, 50, 40));  // slot 0

    // Split 20 off slot0 into empty slot2: source 30, new stack 20 (unminted).
    inv.split_backpack(0, 2, 20);
    check("split leaves 30 in the source", inv.backpack_at(0)->stack == 30);
    check("split creates a 20 stack in the target", inv.backpack_at(2)->stack == 20);
    check("split-off stack is unminted (guid 0)", inv.backpack_at(2)->item_guid == 0);

    check("split the whole stack throws BadStackCount",
          throws<BadStackCount>([&] { inv.split_backpack(0, 1, 30); }));
    check("split into an occupied slot throws SlotOccupied",
          throws<SlotOccupied>([&] { inv.split_backpack(0, 2, 5); }));

    // Non-stackable items cannot be split.
    inv.add(make_inst(kSword, 1, 41));  // slot 1
    check("split a non-stackable item throws NotStackable",
          throws<NotStackable>([&] { inv.split_backpack(1, 3, 1); }));

    // Merge slot2 (20) back into slot0 (30) -> 50, slot2 cleared.
    inv.merge_backpack(2, 0);
    check("merge sums the stacks", inv.backpack_at(0)->stack == 50);
    check("fully-absorbed source slot clears", inv.backpack_at(2) == nullptr);

    // Merge across different templates is refused.
    check("merge different templates throws NotStackable",
          throws<NotStackable>([&] { inv.merge_backpack(0, 1); }));

    // Move: relocate to an empty slot, then swap two occupied slots.
    inv.move_backpack(0, 3);
    check("move to empty relocates the stack",
          inv.backpack_at(0) == nullptr && inv.backpack_at(3)->template_id == kOre);
    inv.move_backpack(1, 3);  // slot1 = sword, slot3 = ore -> swap
    check("move to occupied swaps (sword now in slot3)",
          inv.backpack_at(3)->template_id == kSword &&
          inv.backpack_at(1)->template_id == kOre);
}

// ---- inventory: equip validation + slot rules -------------------------------
void test_equip() {
    std::printf("[inventory equip]\n");
    PlaceholderTemplateStore store;
    Inventory inv(store, /*backpack_capacity=*/8);

    inv.add(make_inst(kSword, 1, 50));    // 0
    inv.add(make_inst(kBuckler, 1, 51));  // 1
    inv.add(make_inst(kStaff, 1, 52));    // 2
    inv.add(make_inst(kRing, 1, 53));     // 3 (requires level 5)
    inv.add(make_inst(kPotion, 5, 54));   // 4 (not equippable)

    // A consumable cannot be equipped.
    check("equip a non-equippable item throws NotEquippable",
          throws<NotEquippable>([&] { inv.equip_from_backpack(4, 10); }));

    // Level gate: the ring requires level 5.
    check("equip below required level throws LevelTooLow",
          throws<LevelTooLow>([&] { inv.equip_from_backpack(3, 1); }));

    // Equip the sword (main hand) at level 1 — backpack slot frees.
    inv.equip_from_backpack(0, 1);
    check("sword is now equipped in the main hand",
          inv.equipped_at(EquipSlot::kMainHand) &&
          inv.equipped_at(EquipSlot::kMainHand)->template_id == kSword);
    check("the sword's backpack slot is now free", inv.backpack_at(0) == nullptr);

    // Equip the buckler (off hand).
    inv.equip_from_backpack(1, 1);
    check("buckler is equipped in the off hand",
          inv.equipped_at(EquipSlot::kOffHand) &&
          inv.equipped_at(EquipSlot::kOffHand)->template_id == kBuckler);

    // Two-hand rule: cannot equip the staff while the off hand is occupied.
    check("equip two-hand with off hand occupied throws TwoHandNeedsOffHandEmpty",
          throws<TwoHandNeedsOffHandEmpty>([&] { inv.equip_from_backpack(2, 1); }));

    // Free the off hand, then the staff equips (displacing the sword to backpack).
    inv.unequip(EquipSlot::kOffHand);
    inv.equip_from_backpack(2, 1);
    check("staff is now in the main hand",
          inv.equipped_at(EquipSlot::kMainHand)->template_id == kStaff);
    check("the displaced sword returned to the backpack",
          inv.backpack_at(2) && inv.backpack_at(2)->template_id == kSword);

    // Off-hand rule: cannot fill the off hand while a two-hand item is main-hand.
    // The buckler is back in the backpack (unequipped above) — find it and try.
    std::uint16_t buckler_slot = 0xFFFF;
    for (std::uint16_t i = 0; i < inv.backpack_capacity(); ++i) {
        const ItemInstance* it = inv.backpack_at(i);
        if (it && it->template_id == kBuckler) { buckler_slot = i; break; }
    }
    check("buckler was located in the backpack", buckler_slot != 0xFFFF);
    check("equip off-hand while a two-hand is worn throws OffHandBlockedByTwoHand",
          throws<OffHandBlockedByTwoHand>(
              [&] { inv.equip_from_backpack(buckler_slot, 1); }));

    // Unequip into the backpack; unequipping an empty slot is refused.
    inv.unequip(EquipSlot::kMainHand);
    check("main hand is empty after unequip",
          inv.equipped_at(EquipSlot::kMainHand) == nullptr);
    check("unequip an empty slot throws SlotEmpty",
          throws<SlotEmpty>([&] { inv.unequip(EquipSlot::kMainHand); }));
}

// ---- inventory: unequip into a full backpack --------------------------------
void test_unequip_full() {
    std::printf("[inventory unequip when full]\n");
    PlaceholderTemplateStore store;
    Inventory inv(store, /*backpack_capacity=*/1);

    inv.add(make_inst(kSword, 1, 60));  // fills the single slot
    inv.equip_from_backpack(0, 1);      // main hand; backpack now free
    inv.add(make_inst(kVest, 1, 61));   // refill the single slot

    // The sword is equipped, the one backpack slot is taken -> no room to unequip.
    check("unequip with a full backpack throws InventoryFull",
          throws<InventoryFull>([&] { inv.unequip(EquipSlot::kMainHand); }));
    check("the sword is still equipped after the failed unequip",
          inv.equipped_at(EquipSlot::kMainHand) != nullptr);
}

}  // namespace

int main() {
    test_templates();
    test_currency();
    test_add_remove();
    test_stacking();
    test_split_merge_move();
    test_equip();
    test_unequip_full();

    std::printf(g_fail == 0 ? "\nALL ITEM UNIT TESTS PASSED\n"
                            : "\n%d ITEM UNIT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
