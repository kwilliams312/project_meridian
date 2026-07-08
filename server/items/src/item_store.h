// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — characters-DB persistence for item INSTANCES and their
// PLACEMENT (ITM-01; server PRD §4-M1 "instances persist to the characters DB";
// issue #366).
//
// This is the DB seam between the pure inventory model (inventory.h) and the
// durable characters DB (server/db/characters/migrations/0001):
//   * item_instance      — one row per concrete item (server-minted item_guid).
//   * character_inventory — placement: (char_id, bag, slot) -> item_guid.
// Parameterized SQL only (meridian-db prepared statements — user input is NEVER
// concatenated into SQL). All 64-bit ids bind as decimal strings (the meridian-db
// BIGINT-UNSIGNED signed-bind gotcha, documented in characters.cpp).
//
// PLACEMENT SLOT MAPPING (inventory.h slot model) — everything is bag 0 in M1:
//   * equipment paperdoll slot  ->  character_inventory.slot = EquipSlot value
//                                   ([0, kEquipSlotCount)).
//   * backpack index i          ->  character_inventory.slot = kBackpackSlotBase+i.
// load_inventory reverses this mapping.
//
// The daemon-facing operations are granular PRIMITIVES (mint / destroy /
// set-stack / place / move / clear / load) rather than a monolithic "save": loot
// (#369) mints-then-places on a drop; a vendor (#370) mints-then-places on a buy
// and clears-then-destroys on a sell. Each primitive is one statement (or a small
// documented transaction) so consumers compose the transaction boundary they need.
//
// Clean-room, original code (CONTRIBUTING.md).

#pragma once

#include <cstdint>

#include "inventory.h"
#include "item_template.h"
#include "meridian/db/connection.h"

namespace meridian::items {

// The character_inventory.slot number for a paperdoll position (bag 0).
inline std::uint16_t equip_placement_slot(EquipSlot slot) {
    return static_cast<std::uint16_t>(slot);
}

// The character_inventory.slot number for backpack index `i` (bag 0).
inline std::uint16_t backpack_placement_slot(std::uint16_t i) {
    return static_cast<std::uint16_t>(kBackpackSlotBase + i);
}

// Mint a brand-new item_instance row and return it with its server-assigned
// item_guid (the AUTO_INCREMENT last_insert_id). `stack` must be >= 1. `creator`
// 0 persists as SQL NULL (no crafter). The minted instance is NOT yet placed —
// the caller places it (place_item) inside its own transaction. Throws
// db::DbError on failure.
ItemInstance mint_instance(db::Connection& conn, std::uint32_t template_id,
                           std::uint32_t stack, std::uint64_t creator = 0,
                           std::uint32_t suffix_id = 0);

// Update the stack count of an existing instance (after a split/merge/partial
// remove). Returns true iff a row was updated.
bool set_instance_stack(db::Connection& conn, std::uint64_t item_guid,
                        std::uint32_t stack);

// Permanently delete an instance row. The schema's ON DELETE CASCADE on
// character_inventory.item_guid removes any placement pointing at it. Returns
// true iff a row was deleted.
bool destroy_instance(db::Connection& conn, std::uint64_t item_guid);

// Place `item_guid` at (char_id, bag, slot). INSERTs a character_inventory row;
// the schema's UNIQUE (char_id,bag,slot) and UNIQUE (item_guid) reject a double
// placement or a slot collision (surfaced as db::DbError). Use the *_placement_slot
// helpers above to compute `slot` from an EquipSlot / backpack index.
void place_item(db::Connection& conn, std::uint64_t char_id, std::uint8_t bag,
                std::uint16_t slot, std::uint64_t item_guid);

// Move an already-placed instance to a new (bag, slot) for the same character.
// Returns true iff the placement row was updated. Throws db::DbError if the
// destination slot is occupied (UNIQUE (char_id,bag,slot)).
bool move_placement(db::Connection& conn, std::uint64_t char_id, std::uint8_t bag,
                    std::uint16_t slot, std::uint64_t item_guid);

// Remove the placement of `item_guid` for `char_id` (the item leaves the grid but
// the instance row survives — e.g. escrow/trade). Returns true iff a row was
// removed. Ownership is the query: another character's placement matches nothing.
bool clear_placement(db::Connection& conn, std::uint64_t char_id,
                     std::uint64_t item_guid);

// Load a character's full inventory (equipment + backpack) from the characters
// DB into an in-memory Inventory, using `templates` for validation-free
// reconstruction (the DB is the durable arbiter — §4.7). Reads every bag-0
// placement JOINed to its instance and maps the slot number back to a paperdoll
// position or backpack index. Throws db::DbError on failure.
Inventory load_inventory(db::Connection& conn, std::uint64_t char_id,
                         const TemplateStore& templates,
                         std::uint16_t backpack_capacity = kDefaultBackpackSlots);

}  // namespace meridian::items
