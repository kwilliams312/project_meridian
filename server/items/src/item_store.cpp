// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — characters-DB persistence for item instances + placement
// (ITM-01; issue #366). See item_store.h for the slot mapping + primitive-set
// rationale.

#include "item_store.h"

#include <string>

namespace meridian::items {

namespace {

// Bind a 64-bit UNSIGNED id as a DECIMAL STRING (meridian-db signed-bind gotcha;
// see characters.cpp). item_guid / char_id / creator are BIGINT UNSIGNED.
db::Param bind_id(std::uint64_t v) { return db::Param{std::to_string(v)}; }

// Bind a small unsigned value (template_id / stack / durability / suffix / bag /
// slot). All fit comfortably in a signed int64, so the signed LONGLONG path is
// safe here (unlike the 64-bit ids above).
db::Param bind_u(std::uint64_t v) { return db::Param{static_cast<std::int64_t>(v)}; }

std::uint64_t parse_u64(const db::Cell& c) {
    if (!c.has_value() || c->empty()) return 0;
    return std::stoull(*c);
}

std::uint32_t parse_u32(const db::Cell& c) {
    if (!c.has_value() || c->empty()) return 0;
    return static_cast<std::uint32_t>(std::stoul(*c));
}

}  // namespace

ItemInstance mint_instance(db::Connection& conn, std::uint32_t template_id,
                           std::uint32_t stack, std::uint64_t creator,
                           std::uint32_t suffix_id) {
    // creator 0 -> SQL NULL (no crafter); the column is nullable + SET NULL.
    db::Param creator_param =
        creator == 0 ? db::Param{std::monostate{}} : bind_id(creator);

    db::Result r = conn.execute(
        "INSERT INTO item_instance "
        "(item_template_id, stack, suffix_id, creator) VALUES (?, ?, ?, ?)",
        {bind_u(template_id), bind_u(stack), bind_u(suffix_id), creator_param});

    ItemInstance inst;
    inst.item_guid = r.last_insert_id;  // server-minted durable identity
    inst.template_id = template_id;
    inst.stack = stack;
    inst.suffix_id = suffix_id;
    inst.creator = creator;
    return inst;
}

bool set_instance_stack(db::Connection& conn, std::uint64_t item_guid,
                        std::uint32_t stack) {
    db::Result r = conn.execute(
        "UPDATE item_instance SET stack = ? WHERE item_guid = ?",
        {bind_u(stack), bind_id(item_guid)});
    return r.affected_rows == 1;
}

bool destroy_instance(db::Connection& conn, std::uint64_t item_guid) {
    db::Result r = conn.execute(
        "DELETE FROM item_instance WHERE item_guid = ?", {bind_id(item_guid)});
    return r.affected_rows == 1;
}

void place_item(db::Connection& conn, std::uint64_t char_id, std::uint8_t bag,
                std::uint16_t slot, std::uint64_t item_guid) {
    conn.execute(
        "INSERT INTO character_inventory (char_id, bag, slot, item_guid) "
        "VALUES (?, ?, ?, ?)",
        {bind_id(char_id), bind_u(bag), bind_u(slot), bind_id(item_guid)});
}

bool move_placement(db::Connection& conn, std::uint64_t char_id, std::uint8_t bag,
                    std::uint16_t slot, std::uint64_t item_guid) {
    db::Result r = conn.execute(
        "UPDATE character_inventory SET bag = ?, slot = ? "
        "WHERE char_id = ? AND item_guid = ?",
        {bind_u(bag), bind_u(slot), bind_id(char_id), bind_id(item_guid)});
    return r.affected_rows == 1;
}

bool clear_placement(db::Connection& conn, std::uint64_t char_id,
                     std::uint64_t item_guid) {
    db::Result r = conn.execute(
        "DELETE FROM character_inventory WHERE char_id = ? AND item_guid = ?",
        {bind_id(char_id), bind_id(item_guid)});
    return r.affected_rows == 1;
}

Inventory load_inventory(db::Connection& conn, std::uint64_t char_id,
                         const TemplateStore& templates,
                         std::uint16_t backpack_capacity) {
    Inventory inv(templates, backpack_capacity);

    db::Result r = conn.execute(
        "SELECT ci.bag, ci.slot, ii.item_guid, ii.item_template_id, ii.stack, "
        "       ii.durability, ii.suffix_id, ii.creator "
        "FROM character_inventory ci "
        "JOIN item_instance ii ON ii.item_guid = ci.item_guid "
        "WHERE ci.char_id = ? ORDER BY ci.bag, ci.slot",
        {bind_id(char_id)});

    for (const db::Row& row : r.rows) {
        const std::uint32_t bag = parse_u32(row[0]);
        const std::uint16_t slot = static_cast<std::uint16_t>(parse_u32(row[1]));

        ItemInstance inst;
        inst.item_guid = parse_u64(row[2]);
        inst.template_id = parse_u32(row[3]);
        inst.stack = parse_u32(row[4]);
        inst.durability = parse_u32(row[5]);
        inst.suffix_id = parse_u32(row[6]);
        inst.creator = parse_u64(row[7]);

        // M1: only bag 0 exists (equipped set + backpack). bag>0 containers are
        // an M2 feature — skip defensively rather than fail a load.
        if (bag != 0) continue;

        if (slot < kEquipSlotCount) {
            inv.load_equipment(static_cast<EquipSlot>(slot), inst);
        } else if (slot >= kBackpackSlotBase) {
            inv.load_backpack(static_cast<std::uint16_t>(slot - kBackpackSlotBase), inst);
        }
        // A slot in the gap [kEquipSlotCount, kBackpackSlotBase) is not part of
        // the M1 layout; ignore it (defensive against a hand-edited row).
    }
    return inv;
}

}  // namespace meridian::items
