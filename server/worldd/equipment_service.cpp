// SPDX-License-Identifier: Apache-2.0

#include "equipment_service.h"

#include <optional>
#include <string>
#include <utility>

#include "item_store.h"

namespace meridian::worldd {
namespace {

db::Param bind_id(std::uint64_t value) { return db::Param{std::to_string(value)}; }

class Transaction {
public:
    explicit Transaction(db::Connection& conn) : conn_(conn) {
        conn_.execute("START TRANSACTION");
    }
    ~Transaction() {
        if (!done_) {
            try { conn_.execute("ROLLBACK"); } catch (...) {}
        }
    }
    void commit() {
        conn_.execute("COMMIT");
        done_ = true;
    }
private:
    db::Connection& conn_;
    bool done_ = false;
};

void lock_inventory(db::Connection& conn, std::uint64_t char_id) {
    // Serialize every placement mutation for this character. The selected rows are
    // owned by char_id, so a client can never name or lock another character's item.
    conn.execute("SELECT item_guid FROM character_inventory WHERE char_id = ? FOR UPDATE",
                 {bind_id(char_id)});
}

void replace_placements(db::Connection& conn, std::uint64_t char_id,
                        const items::ItemInstance& incoming,
                        std::uint16_t incoming_destination,
                        const items::ItemInstance* displaced,
                        std::uint16_t displaced_destination) {
    if (!items::clear_placement(conn, char_id, incoming.item_guid))
        throw items::SlotEmpty();
    if (displaced != nullptr &&
        !items::clear_placement(conn, char_id, displaced->item_guid))
        throw items::SlotEmpty();
    items::place_item(conn, char_id, 0, incoming_destination, incoming.item_guid);
    if (displaced != nullptr)
        items::place_item(conn, char_id, 0, displaced_destination, displaced->item_guid);
}

}  // namespace

EquipmentMutationResult equip_owned_item(
    db::Connection& conn, std::uint64_t char_id, std::uint16_t backpack_slot,
    std::uint16_t char_level, const ClassRecord& char_class,
    const EquipTypeCatalog& equip_types, const items::TemplateStore& templates) {
    Transaction tx(conn);
    lock_inventory(conn, char_id);
    items::Inventory inv = items::load_inventory(conn, char_id, templates);

    const items::ItemInstance* source = inv.backpack_at(backpack_slot);
    if (source == nullptr) throw items::SlotEmpty();
    const items::ItemTemplate* item = templates.find(source->template_id);
    if (item == nullptr) throw items::UnknownTemplate(source->template_id);
    if (!item->is_equippable()) throw items::NotEquippable();
    const std::optional<items::EquipSlot> target = items::equip_slot_for(item->slot);
    if (!target) throw items::NotEquippable();

    const EquipGate gate = gate_equip(char_class, equip_types, *item);
    if (gate != EquipGate::kAllowed) throw EquipGateRejected(gate);

    const items::ItemInstance incoming = *source;
    const items::ItemInstance* old = inv.equipped_at(*target);
    const std::optional<items::ItemInstance> displaced =
        old == nullptr ? std::nullopt : std::optional<items::ItemInstance>(*old);

    // Pure validation runs before durable writes (level, hand conflicts, slot rules).
    inv.equip_from_backpack(backpack_slot, char_level);
    replace_placements(conn, char_id, incoming, items::equip_placement_slot(*target),
                       displaced ? &*displaced : nullptr,
                       items::backpack_placement_slot(backpack_slot));
    tx.commit();
    return EquipmentMutationResult{*target, std::move(inv)};
}

EquipmentMutationResult unequip_owned_item(
    db::Connection& conn, std::uint64_t char_id, items::EquipSlot equipped_slot,
    const items::TemplateStore& templates) {
    if (static_cast<std::size_t>(equipped_slot) >= items::kEquipSlotCount)
        throw items::InvalidSlot("paperdoll slot");

    Transaction tx(conn);
    lock_inventory(conn, char_id);
    items::Inventory inv = items::load_inventory(conn, char_id, templates);
    const items::ItemInstance* equipped = inv.equipped_at(equipped_slot);
    if (equipped == nullptr) throw items::SlotEmpty();
    const items::ItemInstance moving = *equipped;

    inv.unequip(equipped_slot);  // validates backpack capacity before writes
    std::uint16_t destination = 0;
    for (; destination < inv.backpack_capacity(); ++destination) {
        const items::ItemInstance* row = inv.backpack_at(destination);
        if (row != nullptr && row->item_guid == moving.item_guid) break;
    }
    if (destination >= inv.backpack_capacity()) throw items::InventoryFull();
    replace_placements(conn, char_id, moving,
                       items::backpack_placement_slot(destination), nullptr, 0);
    tx.commit();
    return EquipmentMutationResult{equipped_slot, std::move(inv)};
}

}  // namespace meridian::worldd
