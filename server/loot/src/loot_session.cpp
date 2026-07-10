// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — the corpse loot window (ITM-02; issue #369).
// See loot_session.h for the design + clean-room provenance.

#include "loot_session.h"

#include <algorithm>
#include <cmath>

namespace meridian::loot {

float loot_distance(const LootPoint& a, const LootPoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

LootSession::LootSession(LooterId corpse_guid, const LootPoint& corpse_pos, LootRoll roll,
                         std::vector<LooterId> owners, float loot_range)
    : corpse_guid_(corpse_guid),
      corpse_pos_(corpse_pos),
      roll_(std::move(roll)),
      owners_(std::move(owners)),
      loot_range_(loot_range),
      common_taken_(roll_.stacks.size(), false),
      quest_taken_by_(roll_.stacks.size()) {}

bool LootSession::is_owner(LooterId looter) const {
    return std::find(owners_.begin(), owners_.end(), looter) != owners_.end();
}

bool LootSession::slot_taken_by(std::size_t slot, LooterId looter) const {
    if (slot >= roll_.stacks.size()) return false;
    if (roll_.stacks[slot].is_quest())
        return quest_taken_by_[slot].count(looter) != 0;
    return common_taken_[slot];
}

std::vector<LootSlotView> LootSession::visible_slots(LooterId looter,
                                                     const QuestPredicate& has_quest) const {
    std::vector<LootSlotView> out;
    if (!is_owner(looter)) return out;  // a non-owner sees nothing
    for (std::size_t i = 0; i < roll_.stacks.size(); ++i) {
        const LootStack& s = roll_.stacks[i];
        if (s.is_quest()) {
            // Quest slot: visible only to an eligible looter who has not yet taken
            // their personal copy.
            if (!has_quest || !has_quest(s.required_quest_id)) continue;
            if (quest_taken_by_[i].count(looter) != 0) continue;
        } else {
            if (common_taken_[i]) continue;  // shared, already gone
        }
        out.push_back(LootSlotView{i, s.item_template_id, s.count, s.required_quest_id});
    }
    return out;
}

void LootSession::require_owner_in_range(LooterId looter, const LootPoint& looter_pos) const {
    if (!is_owner(looter)) throw NotAnOwner();
    if (loot_distance(looter_pos, corpse_pos_) > loot_range_) throw LootOutOfRange();
}

LootStack LootSession::take_item(LooterId looter, const LootPoint& looter_pos, std::size_t slot,
                                 const QuestPredicate& has_quest, items::Inventory& inv) {
    require_owner_in_range(looter, looter_pos);
    if (slot >= roll_.stacks.size())
        throw LootSlotInvalid(std::to_string(slot) + " >= " +
                              std::to_string(roll_.stacks.size()));

    const LootStack& s = roll_.stacks[slot];

    // Quest eligibility BEFORE the already-taken check, so an ineligible looter is
    // told QUEST_REQUIRED (not "already taken" — they never had access).
    if (s.is_quest()) {
        if (!has_quest || !has_quest(s.required_quest_id))
            throw LootQuestRequired(s.required_quest_id);
        if (quest_taken_by_[slot].count(looter) != 0) throw LootAlreadyTaken();
    } else {
        if (common_taken_[slot]) throw LootAlreadyTaken();
    }

    // Transfer into the inventory FIRST (all-or-nothing): if there is no room this
    // throws items::InventoryFull and the session state below is never touched —
    // the loot stays on the corpse. The instance is UNMINTED (guid 0): the durable
    // item_instance row is minted by the caller's DB transaction on persist (the
    // mint-then-place seam, item_store.h) — the pure inventory model carries it as
    // a fresh stack.
    items::ItemInstance inst;
    inst.item_guid = 0;
    inst.template_id = s.item_template_id;
    inst.stack = s.count;
    inv.add(inst);

    // Committed — mark the slot taken (shared or personal).
    if (s.is_quest())
        quest_taken_by_[slot].insert(looter);
    else
        common_taken_[slot] = true;

    return s;
}

items::Copper LootSession::take_money(LooterId looter, const LootPoint& looter_pos) {
    require_owner_in_range(looter, looter_pos);
    if (roll_.copper <= 0 || copper_taken_) throw LootAlreadyTaken();
    copper_taken_ = true;
    return roll_.copper;
}

bool LootSession::fully_looted() const {
    if (roll_.copper > 0 && !copper_taken_) return false;
    for (std::size_t i = 0; i < roll_.stacks.size(); ++i) {
        if (roll_.stacks[i].is_quest()) continue;  // personal — never blocks despawn
        if (!common_taken_[i]) return false;
    }
    return true;
}

}  // namespace meridian::loot
