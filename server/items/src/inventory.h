// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — item INSTANCES + the server-authoritative inventory/equip
// container (ITM-01; server PRD §4-M1 "bags/inventory ops with server-side
// validation (no client-trusted moves), equip rules"; issue #366).
//
// An ItemInstance is a CONCRETE item: a server-minted guid, a stack count, and
// its template id (the static definition, item_template.h). Instances persist as
// characters-DB `item_instance` rows; their PLACEMENT (which slot holds which
// instance) persists as `character_inventory` rows (item_store.h mints/loads/
// saves — this file is pure, DB-free, deterministic domain logic).
//
// SLOT MODEL — from the DDL (0001_init_characters.up.sql character_inventory):
//   character_inventory maps (char_id, bag, slot) -> item_guid, "bag 0 =
//   backpack/equipped set". This container lives entirely in bag 0 and splits it
//   into two disjoint slot ranges (item_store.h persists exactly this mapping):
//     * EQUIPMENT (the paperdoll): slots [0, kEquipSlotCount)  — one per EquipSlot.
//     * BACKPACK:                  slots [kBackpackSlotBase, base + capacity) —
//                                  the loose-item grid.
//   (bag > 0 — items that are themselves containers — is an M2 feature and is
//   deliberately not modelled here.)
//
// SERVER-AUTHORITATIVE: every mutation is validated here. A client asks to move/
// equip/split; the server decides. Illegal requests throw a typed error and
// leave the inventory UNCHANGED (operations are all-or-nothing). This is the
// single validation path the PRD requires loot (#369), vendors (#370) and later
// bank/trade (ECO-05/SOC-03) to share.
//
// Clean-room, original code (CONTRIBUTING.md — no GPL/leaked source).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "item_template.h"

namespace meridian::items {

// A concrete item (characters-DB `item_instance` row). Instances of a stackable
// template are FUNGIBLE — one row carries the whole stack.
struct ItemInstance {
    std::uint64_t item_guid = 0;    // server-minted durable identity; 0 = not yet minted
    std::uint32_t template_id = 0;  // -> world DB item_template (IF-9)
    std::uint32_t stack = 1;        // units in this stack (>=1; <= template.max_stack)
    std::uint32_t durability = 0;   // remaining durability (0 = n/a; model is ITM-03/M2)
    std::uint32_t suffix_id = 0;    // random-suffix ref (0 = none; ITM-03/M2)
    std::uint64_t creator = 0;      // crafting character.id (0 = none)
};

// The character's paperdoll POSITIONS (distinct from an item's ItemSlot type). A
// two-hand item (ItemSlot::kTwoHand) is worn in kMainHand and blocks kOffHand.
// The values are contiguous from 0 so they double as the persisted equipment
// slot numbers (see kEquipSlotCount / item_store.h).
enum class EquipSlot : std::uint8_t {
    kHead = 0,
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
    kRanged,
};

// Number of paperdoll positions — the size of the equipment array and the count
// of reserved low slot numbers in bag 0.
inline constexpr std::size_t kEquipSlotCount =
    static_cast<std::size_t>(EquipSlot::kRanged) + 1;

// First backpack slot number in bag 0. Backpack slots are [base, base+capacity).
// Chosen well above kEquipSlotCount so the equipment and backpack slot ranges
// never overlap in the persisted (bag 0) namespace.
inline constexpr std::uint16_t kBackpackSlotBase = 100;

// Default backpack capacity (loose-item grid size) for a new character. Named so
// it is one place to change; the ctor accepts an override.
inline constexpr std::uint16_t kDefaultBackpackSlots = 16;

// Map an item's ItemSlot type to the paperdoll position it occupies, or nullopt
// if the item is not equippable (kNone / kBag). kTwoHand maps to kMainHand.
std::optional<EquipSlot> equip_slot_for(ItemSlot slot);

// --- Errors ------------------------------------------------------------------
// Every rejected operation throws exactly one of these; the inventory is left
// unchanged. Callers (a worldd handler, loot, a vendor) map each to a protocol
// status. Grouped under a common base so a caller may catch broadly.

class InventoryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A slot index/position was out of range for this inventory.
class InvalidSlot : public InventoryError {
public:
    explicit InvalidSlot(const std::string& what)
        : InventoryError("invalid slot: " + what) {}
};

// The target slot already holds an item (and the op needed it empty).
class SlotOccupied : public InventoryError {
public:
    SlotOccupied() : InventoryError("target slot is occupied") {}
};

// The source slot is empty (nothing to move/split/equip/remove).
class SlotEmpty : public InventoryError {
public:
    SlotEmpty() : InventoryError("source slot is empty") {}
};

// No room to place the item(s) (all relevant backpack slots full).
class InventoryFull : public InventoryError {
public:
    InventoryFull() : InventoryError("inventory is full") {}
};

// The template id is not known to the template store.
class UnknownTemplate : public InventoryError {
public:
    explicit UnknownTemplate(std::uint32_t id)
        : InventoryError("unknown item template: " + std::to_string(id)) {}
};

// A stack count was zero, or exceeded the template's max_stack.
class BadStackCount : public InventoryError {
public:
    explicit BadStackCount(const std::string& why)
        : InventoryError("bad stack count: " + why) {}
};

// Split/merge across two DIFFERENT templates, or on a non-stackable item.
class NotStackable : public InventoryError {
public:
    NotStackable() : InventoryError("item is not stackable / templates differ") {}
};

// The item cannot be equipped at all (no paperdoll slot — consumable/quest/etc.).
class NotEquippable : public InventoryError {
public:
    NotEquippable() : InventoryError("item is not equippable") {}
};

// The character's level is below the template's required_level to equip.
class LevelTooLow : public InventoryError {
public:
    LevelTooLow(std::uint16_t required, std::uint16_t have)
        : InventoryError("requires level " + std::to_string(required) +
                        " (character is level " + std::to_string(have) + ")"),
          required_(required), have_(have) {}
    std::uint16_t required() const { return required_; }
    std::uint16_t have() const { return have_; }

private:
    std::uint16_t required_;
    std::uint16_t have_;
};

// Equipping a two-hand item while the off hand is occupied.
class TwoHandNeedsOffHandEmpty : public InventoryError {
public:
    TwoHandNeedsOffHandEmpty()
        : InventoryError("cannot equip a two-hand item with the off hand occupied") {}
};

// Equipping an off-hand item while a two-hand item occupies the main hand.
class OffHandBlockedByTwoHand : public InventoryError {
public:
    OffHandBlockedByTwoHand()
        : InventoryError("off hand is blocked by an equipped two-hand item") {}
};

// --- The container -----------------------------------------------------------

class Inventory {
public:
    // `templates` must outlive the inventory (owned by the datastore seam).
    // `backpack_capacity` is the loose grid size (>=1).
    explicit Inventory(const TemplateStore& templates,
                       std::uint16_t backpack_capacity = kDefaultBackpackSlots);

    // --- queries -------------------------------------------------------------
    std::uint16_t backpack_capacity() const { return backpack_capacity_; }
    std::uint16_t backpack_used() const;
    std::uint16_t backpack_free() const {
        return static_cast<std::uint16_t>(backpack_capacity_ - backpack_used());
    }

    // The instance at a backpack index / paperdoll position, or nullptr if empty.
    const ItemInstance* backpack_at(std::uint16_t index) const;
    const ItemInstance* equipped_at(EquipSlot slot) const;

    // Read-only views for persistence (item_store.h) and inspection.
    const std::vector<std::optional<ItemInstance>>& backpack() const { return backpack_; }
    const std::array<std::optional<ItemInstance>, kEquipSlotCount>& equipment() const {
        return equipment_;
    }

    // --- mutations (server-validated) ---------------------------------------

    // Add `inst` to the backpack. For a stackable template, first tops up existing
    // partial stacks of the same template (fungible), then places any remainder in
    // a free slot. Requires 1 <= inst.stack <= template.max_stack. All-or-nothing:
    // if it cannot fully fit, throws InventoryFull and changes nothing. Throws
    // UnknownTemplate / BadStackCount on a bad request.
    void add(const ItemInstance& inst);

    // Remove `count` units from the backpack item at `index`. Partial removal
    // decrements the stack (the instance/guid stays); removing the whole stack
    // clears the slot. Returns a copy of what was removed (template_id + count +
    // the affected guid) so the DB layer can decrement/destroy the row. Throws
    // SlotEmpty / InvalidSlot / BadStackCount.
    struct Removed {
        std::uint64_t item_guid = 0;
        std::uint32_t template_id = 0;
        std::uint32_t count = 0;
        bool slot_emptied = false;  // true if the stack hit zero and the slot cleared
    };
    Removed remove_from_backpack(std::uint16_t index, std::uint32_t count);

    // Move/swap backpack contents between two indices. If `to` is empty the item
    // relocates; if occupied the two slots swap. (Stack merging is an explicit op,
    // merge_backpack.) Throws SlotEmpty / InvalidSlot.
    void move_backpack(std::uint16_t from, std::uint16_t to);

    // Split `count` units off the stack at `from` into the EMPTY slot `to`. The
    // new stack has guid 0 (unminted — the DB layer mints a fresh item_instance on
    // persist, since a split creates a new row). Requires the template be stackable
    // and 1 <= count < source stack (splitting the whole stack is a move). Throws
    // NotStackable / SlotEmpty / SlotOccupied / BadStackCount / InvalidSlot.
    void split_backpack(std::uint16_t from, std::uint16_t to, std::uint32_t count);

    // Merge the stack at `from` into the stack at `to` (same stackable template),
    // up to max_stack. Overflow beyond max_stack stays at `from`; if `from` is
    // fully absorbed its slot clears (the DB layer destroys the emptied instance).
    // Throws SlotEmpty / NotStackable / InvalidSlot.
    void merge_backpack(std::uint16_t from, std::uint16_t to);

    // Equip the backpack item at `index` onto its paperdoll slot, validating slot
    // compatibility, required_level (vs `char_level`), and the two-hand/off-hand
    // rules. Any item already in the target slot is swapped back into the freed
    // backpack index. All-or-nothing. Throws NotEquippable / LevelTooLow /
    // TwoHandNeedsOffHandEmpty / OffHandBlockedByTwoHand / SlotEmpty / InvalidSlot.
    void equip_from_backpack(std::uint16_t index, std::uint16_t char_level);

    // Unequip the item at paperdoll `slot` into the first free backpack slot.
    // Throws SlotEmpty (nothing equipped) / InventoryFull (no backpack room).
    void unequip(EquipSlot slot);

    // --- trusted load path (item_store.h; NO validation) --------------------
    // Reconstruct persisted state. These bypass validation on purpose: the DB is
    // the durable arbiter of ownership (§4.7) — a loaded placement is already
    // legal. They only guard against range/collision so a corrupt row is caught.
    void load_backpack(std::uint16_t index, const ItemInstance& inst);
    void load_equipment(EquipSlot slot, const ItemInstance& inst);

private:
    std::size_t equip_index(EquipSlot slot) const {
        return static_cast<std::size_t>(slot);
    }
    void require_backpack_index(std::uint16_t index) const;
    const ItemTemplate& require_template(std::uint32_t template_id) const;
    // First free backpack index, or nullopt if the backpack is full.
    std::optional<std::uint16_t> first_free_backpack() const;

    const TemplateStore& templates_;
    std::uint16_t backpack_capacity_;
    std::vector<std::optional<ItemInstance>> backpack_;                 // [0, capacity)
    std::array<std::optional<ItemInstance>, kEquipSlotCount> equipment_;
};

}  // namespace meridian::items
