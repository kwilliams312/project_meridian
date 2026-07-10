// SPDX-License-Identifier: Apache-2.0
//
// worldd — the per-character quest state machine (issue #371, QST-01). Clean-room
// from OUR PRD/SAD + quest.schema.yaml (CONTRIBUTING.md). See quest_log.h.

#include "quest_log.h"

#include <algorithm>

namespace meridian::worldd {

const char* accept_status_name(AcceptStatus s) {
    switch (s) {
        case AcceptStatus::kOk:                   return "ok";
        case AcceptStatus::kUnknownQuest:         return "unknown_quest";
        case AcceptStatus::kAlreadyActive:        return "already_active";
        case AcceptStatus::kAlreadyCompleted:     return "already_completed";
        case AcceptStatus::kLevelTooLow:          return "level_too_low";
        case AcceptStatus::kMissingPrerequisite:  return "missing_prerequisite";
    }
    return "?";
}

const char* turn_in_status_name(TurnInStatus s) {
    switch (s) {
        case TurnInStatus::kOk:            return "ok";
        case TurnInStatus::kUnknownQuest:  return "unknown_quest";
        case TurnInStatus::kNotActive:     return "not_active";
        case TurnInStatus::kWrongNpc:      return "wrong_npc";
        case TurnInStatus::kIncomplete:    return "incomplete";
        case TurnInStatus::kBadChoice:     return "bad_choice";
        case TurnInStatus::kInventoryFull: return "inventory_full";
    }
    return "?";
}

std::uint32_t count_items(const items::Inventory& inv, std::uint32_t template_id) {
    std::uint32_t n = 0;
    for (const auto& slot : inv.backpack())
        if (slot && slot->template_id == template_id) n += slot->stack;
    for (const auto& slot : inv.equipment())
        if (slot && slot->template_id == template_id) n += slot->stack;
    return n;
}

namespace {

// The objective items a turn-in hands over (removed from the inventory): each
// collect objective's item x its goal count, each deliver objective's item x1.
// kill / explore contribute nothing. Deterministic (objective order).
std::vector<QuestRewardItem> objective_consumables(const QuestDef& d) {
    std::vector<QuestRewardItem> out;
    for (const QuestObjective& o : d.objectives) {
        if (o.type == ObjectiveType::kCollect)
            out.push_back(QuestRewardItem{o.item_id, o.count});
        else if (o.type == ObjectiveType::kDeliver)
            out.push_back(QuestRewardItem{o.item_id, std::uint16_t{1}});
    }
    return out;
}

// How many BACKPACK slots removing `want` (item x count each) would empty — a slot
// frees only when the whole stack is taken. Pure (no mutation): lets turn_in judge
// room BEFORE any change so the operation stays all-or-nothing.
std::uint16_t freed_slots_for(const items::Inventory& inv,
                              const std::vector<QuestRewardItem>& want) {
    std::uint16_t freed = 0;
    for (const QuestRewardItem& w : want) {
        std::uint32_t remaining = w.count;
        for (std::uint16_t i = 0; i < inv.backpack_capacity() && remaining > 0; ++i) {
            const items::ItemInstance* inst = inv.backpack_at(i);
            if (inst == nullptr || inst->template_id != w.item_id) continue;
            if (inst->stack <= remaining) { remaining -= inst->stack; ++freed; }
            else { remaining = 0; }
        }
    }
    return freed;
}

// Remove `want` (item x count each) from the backpack, returning what was ACTUALLY
// removed (best-effort — a deliver item completed via the NPC interaction may no
// longer be held). Coalesced per item id.
std::vector<QuestRewardItem> consume_from_backpack(items::Inventory& inv,
                                                   const std::vector<QuestRewardItem>& want) {
    std::vector<QuestRewardItem> removed;
    for (const QuestRewardItem& w : want) {
        std::uint32_t remaining = w.count;
        std::uint32_t got = 0;
        for (std::uint16_t i = 0; i < inv.backpack_capacity() && remaining > 0; ++i) {
            const items::ItemInstance* inst = inv.backpack_at(i);
            if (inst == nullptr || inst->template_id != w.item_id) continue;
            const std::uint32_t take = std::min<std::uint32_t>(remaining, inst->stack);
            const items::Inventory::Removed r = inv.remove_from_backpack(i, take);
            got += r.count;
            remaining -= r.count;
        }
        if (got > 0)
            removed.push_back(QuestRewardItem{w.item_id, static_cast<std::uint16_t>(got)});
    }
    return removed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const std::vector<ObjectiveState>* QuestLog::objectives(QuestId id) const {
    auto it = active_.find(id);
    return it == active_.end() ? nullptr : &it->second.objectives;
}

bool QuestLog::is_objective_complete(QuestId id, std::size_t index) const {
    auto it = active_.find(id);
    if (it == active_.end() || index >= it->second.objectives.size()) return false;
    return it->second.objectives[index].complete();
}

bool QuestLog::is_complete(QuestId id) const {
    auto it = active_.find(id);
    if (it == active_.end()) return false;
    for (const ObjectiveState& o : it->second.objectives)
        if (!o.complete()) return false;
    return true;
}

std::vector<QuestId> QuestLog::active_quests() const {
    std::vector<QuestId> out;
    out.reserve(active_.size());
    for (const auto& [id, _] : active_) out.push_back(id);  // std::map: ascending
    return out;
}

std::vector<QuestId> QuestLog::completed_quests() const {
    return {completed_.begin(), completed_.end()};  // std::set: ascending
}

// ---------------------------------------------------------------------------
// Accept
// ---------------------------------------------------------------------------

AcceptStatus QuestLog::can_accept(QuestId id, std::uint16_t player_level) const {
    const QuestDef* d = def(id);
    if (d == nullptr) return AcceptStatus::kUnknownQuest;
    if (completed_.count(id)) return AcceptStatus::kAlreadyCompleted;
    if (active_.count(id)) return AcceptStatus::kAlreadyActive;
    if (player_level < d->required_level) return AcceptStatus::kLevelTooLow;
    for (QuestId prereq : d->prerequisites)
        if (!completed_.count(prereq)) return AcceptStatus::kMissingPrerequisite;
    return AcceptStatus::kOk;
}

AcceptStatus QuestLog::accept(QuestId id, std::uint16_t player_level) {
    AcceptStatus st = can_accept(id, player_level);
    if (st != AcceptStatus::kOk) return st;

    const QuestDef* d = def(id);  // non-null (can_accept passed)
    Entry e;
    e.objectives.reserve(d->objectives.size());
    for (const QuestObjective& obj : d->objectives)
        e.objectives.push_back(ObjectiveState{/*have=*/0, /*need=*/obj.required()});
    active_.emplace(id, std::move(e));
    return AcceptStatus::kOk;
}

// ---------------------------------------------------------------------------
// Objective progress — the four sources
// ---------------------------------------------------------------------------

bool QuestLog::on_kill(std::uint32_t npc_template_id) {
    bool changed = false;
    for (auto& [id, entry] : active_) {
        const QuestDef* d = def(id);
        if (d == nullptr) continue;
        for (std::size_t i = 0; i < d->objectives.size(); ++i) {
            const QuestObjective& obj = d->objectives[i];
            ObjectiveState& st = entry.objectives[i];
            if (obj.type == ObjectiveType::kKill &&
                obj.target_npc_id == npc_template_id && st.have < st.need) {
                ++st.have;
                changed = true;
            }
        }
    }
    return changed;
}

bool QuestLog::on_explore(std::uint32_t zone_id, const std::string& poi) {
    bool changed = false;
    for (auto& [id, entry] : active_) {
        const QuestDef* d = def(id);
        if (d == nullptr) continue;
        for (std::size_t i = 0; i < d->objectives.size(); ++i) {
            const QuestObjective& obj = d->objectives[i];
            ObjectiveState& st = entry.objectives[i];
            if (obj.type == ObjectiveType::kExplore && obj.zone_id == zone_id &&
                obj.poi == poi && st.have < st.need) {
                st.have = st.need;  // explore is binary
                changed = true;
            }
        }
    }
    return changed;
}

bool QuestLog::on_deliver(std::uint32_t to_npc_id, std::uint32_t item_id) {
    bool changed = false;
    for (auto& [id, entry] : active_) {
        const QuestDef* d = def(id);
        if (d == nullptr) continue;
        for (std::size_t i = 0; i < d->objectives.size(); ++i) {
            const QuestObjective& obj = d->objectives[i];
            ObjectiveState& st = entry.objectives[i];
            if (obj.type == ObjectiveType::kDeliver && obj.to_npc_id == to_npc_id &&
                obj.item_id == item_id && st.have < st.need) {
                st.have = st.need;  // deliver is binary
                changed = true;
            }
        }
    }
    return changed;
}

bool QuestLog::sync_collect(const items::Inventory& inv) {
    bool changed = false;
    for (auto& [id, entry] : active_) {
        const QuestDef* d = def(id);
        if (d == nullptr) continue;
        for (std::size_t i = 0; i < d->objectives.size(); ++i) {
            const QuestObjective& obj = d->objectives[i];
            if (obj.type != ObjectiveType::kCollect) continue;
            ObjectiveState& st = entry.objectives[i];
            std::uint32_t held = count_items(inv, obj.item_id);
            std::uint16_t have = static_cast<std::uint16_t>(std::min<std::uint32_t>(held, st.need));
            if (have != st.have) {
                st.have = have;
                changed = true;
            }
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Turn-in
// ---------------------------------------------------------------------------

TurnInStatus QuestLog::can_turn_in(QuestId id, std::uint32_t npc_id,
                                   int choice_index) const {
    const QuestDef* d = def(id);
    if (d == nullptr) return TurnInStatus::kUnknownQuest;
    if (!active_.count(id)) return TurnInStatus::kNotActive;
    if (npc_id != d->turn_in_npc()) return TurnInStatus::kWrongNpc;
    if (!is_complete(id)) return TurnInStatus::kIncomplete;
    if (!d->choice_items.empty()) {
        if (choice_index < 0 ||
            static_cast<std::size_t>(choice_index) >= d->choice_items.size())
            return TurnInStatus::kBadChoice;
    }
    return TurnInStatus::kOk;
}

TurnInStatus QuestLog::turn_in(QuestId id, std::uint32_t npc_id,
                               items::Inventory& inv, int choice_index,
                               RewardGrant& out) {
    // Refresh collect objectives from the live inventory before judging complete,
    // so a caller need not remember to sync first (the other sources push updates).
    sync_collect(inv);

    TurnInStatus st = can_turn_in(id, npc_id, choice_index);
    if (st != TurnInStatus::kOk) return st;

    const QuestDef* d = def(id);  // non-null (can_turn_in passed)

    // Assemble the reward item list: every always-granted item plus the chosen
    // choice item (if the quest offers a choice).
    std::vector<QuestRewardItem> rewards = d->reward_items;
    if (!d->choice_items.empty())
        rewards.push_back(d->choice_items[static_cast<std::size_t>(choice_index)]);

    // The objective items handed over on turn-in (QST-01): a collect objective's items
    // and a deliver objective's item are CONSUMED from the inventory (the "collect N
    // for X" / "deliver to X" verbs — the character gives them up). Without this the
    // backpack fills across a long quest chain and later turn-ins spuriously report
    // kInventoryFull (the items are never used again). Derived from OUR quest objective
    // semantics (quest.schema.yaml), no GPL source consulted.
    const std::vector<QuestRewardItem> consumables = objective_consumables(*d);

    // Room check (all-or-nothing): reserve one free backpack slot per reward stack,
    // CREDITING the slots the consumption frees. Conservative on the reward side (a
    // stackable reward that tops up an existing stack needs no new slot, so this never
    // under-reserves). Nothing is mutated until this passes.
    const std::uint16_t freed = freed_slots_for(inv, consumables);
    if (static_cast<std::size_t>(inv.backpack_free()) + freed < rewards.size())
        return TurnInStatus::kInventoryFull;

    // Consume the objective items (best-effort; records what was actually removed for
    // the durable layer), then grant: mint each reward stack into the freed backpack
    // (guid 0 = unminted; the DB layer mints a durable item_instance on persist).
    std::vector<QuestRewardItem> consumed = consume_from_backpack(inv, consumables);
    for (const QuestRewardItem& r : rewards) {
        items::ItemInstance inst;
        inst.template_id = r.item_id;
        inst.stack = r.count;
        inv.add(inst);
    }

    active_.erase(id);
    completed_.insert(id);

    out.xp = d->reward_xp;
    out.money = d->reward_money;
    out.items = std::move(rewards);
    out.consumed = std::move(consumed);
    return TurnInStatus::kOk;
}

}  // namespace meridian::worldd
