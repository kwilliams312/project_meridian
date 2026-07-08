// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — inventory/equip container (ITM-01; issue #366).
// See inventory.h for the slot model, server-authoritative rule, and error set.

#include "inventory.h"

#include <algorithm>
#include <utility>

namespace meridian::items {

std::optional<EquipSlot> equip_slot_for(ItemSlot slot) {
    switch (slot) {
        case ItemSlot::kHead:      return EquipSlot::kHead;
        case ItemSlot::kShoulders: return EquipSlot::kShoulders;
        case ItemSlot::kBack:      return EquipSlot::kBack;
        case ItemSlot::kChest:     return EquipSlot::kChest;
        case ItemSlot::kWrist:     return EquipSlot::kWrist;
        case ItemSlot::kHands:     return EquipSlot::kHands;
        case ItemSlot::kWaist:     return EquipSlot::kWaist;
        case ItemSlot::kLegs:      return EquipSlot::kLegs;
        case ItemSlot::kFeet:      return EquipSlot::kFeet;
        case ItemSlot::kNeck:      return EquipSlot::kNeck;
        case ItemSlot::kFinger:    return EquipSlot::kFinger;
        case ItemSlot::kTrinket:   return EquipSlot::kTrinket;
        case ItemSlot::kMainHand:  return EquipSlot::kMainHand;
        case ItemSlot::kOffHand:   return EquipSlot::kOffHand;
        // A two-hand weapon is worn in the main hand (and blocks the off hand).
        case ItemSlot::kTwoHand:   return EquipSlot::kMainHand;
        case ItemSlot::kRanged:    return EquipSlot::kRanged;
        // kNone / kBag are not paperdoll-equippable in M1.
        case ItemSlot::kNone:
        case ItemSlot::kBag:
            return std::nullopt;
    }
    return std::nullopt;
}

Inventory::Inventory(const TemplateStore& templates, std::uint16_t backpack_capacity)
    : templates_(templates),
      backpack_capacity_(backpack_capacity == 0 ? 1 : backpack_capacity),
      backpack_(backpack_capacity_) {}

// --- helpers -----------------------------------------------------------------

void Inventory::require_backpack_index(std::uint16_t index) const {
    if (index >= backpack_capacity_) {
        throw InvalidSlot("backpack index " + std::to_string(index) +
                          " >= capacity " + std::to_string(backpack_capacity_));
    }
}

const ItemTemplate& Inventory::require_template(std::uint32_t template_id) const {
    const ItemTemplate* t = templates_.find(template_id);
    if (t == nullptr) throw UnknownTemplate(template_id);
    return *t;
}

std::optional<std::uint16_t> Inventory::first_free_backpack() const {
    for (std::uint16_t i = 0; i < backpack_capacity_; ++i) {
        if (!backpack_[i].has_value()) return i;
    }
    return std::nullopt;
}

// --- queries -----------------------------------------------------------------

std::uint16_t Inventory::backpack_used() const {
    std::uint16_t n = 0;
    for (const auto& s : backpack_) {
        if (s.has_value()) ++n;
    }
    return n;
}

const ItemInstance* Inventory::backpack_at(std::uint16_t index) const {
    if (index >= backpack_capacity_) return nullptr;
    return backpack_[index].has_value() ? &*backpack_[index] : nullptr;
}

const ItemInstance* Inventory::equipped_at(EquipSlot slot) const {
    const auto& e = equipment_[equip_index(slot)];
    return e.has_value() ? &*e : nullptr;
}

// --- mutations ---------------------------------------------------------------

void Inventory::add(const ItemInstance& inst) {
    const ItemTemplate& tpl = require_template(inst.template_id);
    if (inst.stack == 0) throw BadStackCount("stack must be >= 1");
    if (inst.stack > tpl.max_stack) {
        throw BadStackCount("stack " + std::to_string(inst.stack) +
                            " exceeds max_stack " + std::to_string(tpl.max_stack));
    }

    std::uint32_t remaining = inst.stack;

    if (tpl.is_stackable()) {
        // All-or-nothing: verify total capacity for this template before mutating
        // (partial-stack headroom + free slots * max_stack). Prevents a partial
        // add that tops up a stack then discovers there is no room for the rest.
        std::uint64_t capacity = 0;
        for (const auto& s : backpack_) {
            if (s && s->template_id == inst.template_id && s->stack < tpl.max_stack) {
                capacity += tpl.max_stack - s->stack;
            }
        }
        capacity += static_cast<std::uint64_t>(backpack_free()) * tpl.max_stack;
        if (remaining > capacity) throw InventoryFull();

        // Top up existing partial stacks (fungible units).
        for (auto& s : backpack_) {
            if (remaining == 0) break;
            if (s && s->template_id == inst.template_id && s->stack < tpl.max_stack) {
                std::uint32_t take = std::min<std::uint32_t>(tpl.max_stack - s->stack,
                                                             remaining);
                s->stack += take;
                remaining -= take;
            }
        }
    }

    // Place any remainder. remaining is now <= max_stack (stackable: at most the
    // incoming stack survived topping up; non-stackable: exactly 1), so ONE free
    // slot suffices — the incoming instance (its guid) carries the remainder.
    if (remaining > 0) {
        std::optional<std::uint16_t> free = first_free_backpack();
        if (!free) throw InventoryFull();
        ItemInstance placed = inst;
        placed.stack = remaining;
        backpack_[*free] = placed;
    }
}

Inventory::Removed Inventory::remove_from_backpack(std::uint16_t index,
                                                   std::uint32_t count) {
    require_backpack_index(index);
    auto& slot = backpack_[index];
    if (!slot) throw SlotEmpty();
    if (count == 0) throw BadStackCount("count must be >= 1");
    if (count > slot->stack) {
        throw BadStackCount("count " + std::to_string(count) +
                            " exceeds stack " + std::to_string(slot->stack));
    }

    Removed r;
    r.item_guid = slot->item_guid;
    r.template_id = slot->template_id;
    r.count = count;
    if (count == slot->stack) {
        r.slot_emptied = true;
        slot.reset();
    } else {
        slot->stack -= count;
        r.slot_emptied = false;
    }
    return r;
}

void Inventory::move_backpack(std::uint16_t from, std::uint16_t to) {
    require_backpack_index(from);
    require_backpack_index(to);
    if (from == to) return;  // no-op
    if (!backpack_[from]) throw SlotEmpty();
    // swap covers both cases: `to` empty -> relocate; `to` occupied -> swap.
    std::swap(backpack_[from], backpack_[to]);
}

void Inventory::split_backpack(std::uint16_t from, std::uint16_t to,
                               std::uint32_t count) {
    require_backpack_index(from);
    require_backpack_index(to);
    if (from == to) throw InvalidSlot("split source and target are the same slot");
    auto& src = backpack_[from];
    if (!src) throw SlotEmpty();
    const ItemTemplate& tpl = require_template(src->template_id);
    if (!tpl.is_stackable()) throw NotStackable();
    if (backpack_[to]) throw SlotOccupied();
    if (count == 0 || count >= src->stack) {
        throw BadStackCount("split count must be in [1, stack-1]; stack is " +
                            std::to_string(src->stack));
    }

    // The split-off piece is a NEW stack -> a new item_instance row at persist
    // time, so its guid is 0 (unminted). It inherits the source's non-stack fields.
    ItemInstance piece;
    piece.item_guid = 0;
    piece.template_id = src->template_id;
    piece.stack = count;
    piece.durability = src->durability;
    piece.suffix_id = src->suffix_id;
    piece.creator = src->creator;

    src->stack -= count;
    backpack_[to] = piece;
}

void Inventory::merge_backpack(std::uint16_t from, std::uint16_t to) {
    require_backpack_index(from);
    require_backpack_index(to);
    if (from == to) throw InvalidSlot("merge source and target are the same slot");
    auto& src = backpack_[from];
    auto& dst = backpack_[to];
    if (!src || !dst) throw SlotEmpty();
    if (src->template_id != dst->template_id) throw NotStackable();
    const ItemTemplate& tpl = require_template(dst->template_id);
    if (!tpl.is_stackable()) throw NotStackable();

    std::uint32_t take = std::min<std::uint32_t>(tpl.max_stack - dst->stack, src->stack);
    dst->stack += take;
    src->stack -= take;
    if (src->stack == 0) src.reset();  // fully absorbed -> the DB layer destroys it
}

void Inventory::equip_from_backpack(std::uint16_t index, std::uint16_t char_level) {
    require_backpack_index(index);
    auto& src = backpack_[index];
    if (!src) throw SlotEmpty();

    const ItemTemplate& tpl = require_template(src->template_id);
    if (!tpl.is_equippable()) throw NotEquippable();
    std::optional<EquipSlot> target_opt = equip_slot_for(tpl.slot);
    if (!target_opt) throw NotEquippable();
    const EquipSlot target = *target_opt;

    if (char_level < tpl.required_level) {
        throw LevelTooLow(tpl.required_level, char_level);
    }

    const bool is_two_hand = (tpl.slot == ItemSlot::kTwoHand);
    // Two-hand rule: the off hand must be free to wield a two-hand item.
    if (is_two_hand && equipment_[equip_index(EquipSlot::kOffHand)]) {
        throw TwoHandNeedsOffHandEmpty();
    }
    // Off-hand rule: cannot fill the off hand while a two-hand item is main-hand.
    if (target == EquipSlot::kOffHand) {
        const auto& mh = equipment_[equip_index(EquipSlot::kMainHand)];
        if (mh && require_template(mh->template_id).slot == ItemSlot::kTwoHand) {
            throw OffHandBlockedByTwoHand();
        }
    }

    // Swap: the incoming item goes to the paperdoll; whatever was there returns to
    // the (now-freed) backpack index. 1:1, so no capacity problem.
    ItemInstance to_equip = *src;
    std::optional<ItemInstance> displaced = equipment_[equip_index(target)];
    equipment_[equip_index(target)] = to_equip;
    if (displaced) {
        backpack_[index] = *displaced;
    } else {
        src.reset();
    }
}

void Inventory::unequip(EquipSlot slot) {
    auto& eq = equipment_[equip_index(slot)];
    if (!eq) throw SlotEmpty();
    std::optional<std::uint16_t> free = first_free_backpack();
    if (!free) throw InventoryFull();
    backpack_[*free] = *eq;
    eq.reset();
}

// --- trusted load path -------------------------------------------------------

void Inventory::load_backpack(std::uint16_t index, const ItemInstance& inst) {
    require_backpack_index(index);
    if (backpack_[index]) throw SlotOccupied();
    backpack_[index] = inst;
}

void Inventory::load_equipment(EquipSlot slot, const ItemInstance& inst) {
    auto& eq = equipment_[equip_index(slot)];
    if (eq) throw SlotOccupied();
    eq = inst;
}

}  // namespace meridian::items
