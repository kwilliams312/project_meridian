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

    // Room check (all-or-nothing): reserve one free backpack slot per reward stack.
    // Conservative — a stackable reward that tops up an existing stack needs no new
    // slot, so this never under-reserves; it may reject a turn-in that would fit via
    // top-up, acceptable for the M1 reward sizes. Nothing is mutated on failure.
    if (inv.backpack_free() < rewards.size())
        return TurnInStatus::kInventoryFull;

    // Grant: mint each reward stack into the backpack (guid 0 = unminted; the DB
    // layer mints a durable item_instance on persist, per the split/loot convention).
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
    return TurnInStatus::kOk;
}

}  // namespace meridian::worldd
