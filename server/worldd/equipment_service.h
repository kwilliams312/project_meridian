// SPDX-License-Identifier: Apache-2.0
//
// Authoritative durable equipment mutations (#802). Designed from Meridian's
// inventory contract and SP2 class-kernel specification; no third-party source.

#pragma once

#include <cstdint>
#include <stdexcept>

#include "class_kernel.h"
#include "inventory.h"
#include "item_template.h"
#include "meridian/db/connection.h"

namespace meridian::worldd {

class EquipGateRejected : public std::runtime_error {
public:
    explicit EquipGateRejected(EquipGate gate)
        : std::runtime_error(equip_gate_reason(gate)), gate_(gate) {}
    EquipGate gate() const { return gate_; }
private:
    EquipGate gate_;
};

struct EquipmentMutationResult {
    items::EquipSlot equipped_slot;
    // The exact post-mutation projection produced while the affected rows are
    // locked. Callers use this to reconcile the owner and observers without a
    // fallible second database read after COMMIT.
    items::Inventory inventory;
};

// Equip the owned item in a backpack grid slot. An occupied target is replaced and
// the displaced item returns to the source backpack slot. The complete validation +
// placement rewrite commits atomically; every exception leaves durable state unchanged.
EquipmentMutationResult equip_owned_item(
    db::Connection& conn, std::uint64_t char_id, std::uint16_t backpack_slot,
    std::uint16_t char_level, const ClassRecord& char_class,
    const EquipTypeCatalog& equip_types, const items::TemplateStore& templates);

// Unequip one paperdoll slot into the first free backpack slot, atomically.
EquipmentMutationResult unequip_owned_item(
    db::Connection& conn, std::uint64_t char_id, items::EquipSlot equipped_slot,
    const items::TemplateStore& templates);

}  // namespace meridian::worldd
