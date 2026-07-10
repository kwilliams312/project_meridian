// SPDX-License-Identifier: Apache-2.0
//
// worldd — quest definitions: objective-type naming + the M1 PLACEHOLDER quest
// chain (issue #371, QST-01). Clean-room from quest.schema.yaml / 40_quest.sql
// (CONTRIBUTING.md). See quest_def.h.

#include "quest_def.h"

#include <algorithm>

#include "item_template.h"  // meridian::items::kPlaceholderIdBase (reward/objective item ids)

namespace meridian::worldd {

const char* objective_type_name(ObjectiveType t) {
    switch (t) {
        case ObjectiveType::kKill:    return "kill";
        case ObjectiveType::kCollect: return "collect";
        case ObjectiveType::kDeliver: return "deliver";
        case ObjectiveType::kExplore: return "explore";
    }
    return "?";
}

namespace {

// Placeholder content ids. Quests live in the reserved quest range; the objective
// creature, the deliver-to NPC, the givers and the explore zone are dev-only
// stand-ins (mcc #28 seam). Reward/objective ITEM ids reference the placeholder
// item template set (item_template.cpp) so a granted reward is a real template.
constexpr QuestId Q = kPlaceholderQuestIdBase;
constexpr std::uint32_t N = kPlaceholderNpcIdBase;
constexpr std::uint32_t Z = kPlaceholderZoneIdBase;
constexpr std::uint32_t I = items::kPlaceholderIdBase;

// Giver / turn-in NPCs (offer + hand-in) and the objective creature.
constexpr std::uint32_t kSergeant = N + 1;       // giver: kill + explore quests
constexpr std::uint32_t kSmith    = N + 2;       // giver: collect quest
constexpr std::uint32_t kCourier  = N + 3;       // deliver-to NPC
constexpr std::uint32_t kKobold   = N + 10;      // kill-objective creature template

QuestObjective kill_obj(std::uint32_t npc, std::uint16_t count) {
    QuestObjective o;
    o.type = ObjectiveType::kKill;
    o.target_npc_id = npc;
    o.count = count;
    return o;
}
QuestObjective collect_obj(std::uint32_t item, std::uint16_t count) {
    QuestObjective o;
    o.type = ObjectiveType::kCollect;
    o.item_id = item;
    o.count = count;
    return o;
}
QuestObjective deliver_obj(std::uint32_t item, std::uint32_t to_npc) {
    QuestObjective o;
    o.type = ObjectiveType::kDeliver;
    o.item_id = item;
    o.to_npc_id = to_npc;
    o.count = 1;
    return o;
}
QuestObjective explore_obj(std::uint32_t zone, std::string poi) {
    QuestObjective o;
    o.type = ObjectiveType::kExplore;
    o.zone_id = zone;
    o.poi = std::move(poi);
    o.count = 1;
    return o;
}

}  // namespace

PlaceholderQuestStore::PlaceholderQuestStore() {
    // Q1 — kill: cull 3 kobolds. Entry quest (no gate). Turned in to the giver.
    {
        QuestDef q;
        q.id = Q + 1;
        q.name = "Culling the Kobolds";
        q.level = 2;
        q.required_level = 1;
        q.giver_npc_id = kSergeant;
        q.objectives = {kill_obj(kKobold, 3)};
        q.reward_xp = 50;
        q.reward_money = items::Copper{120};
        q.reward_items = {{I + 7, 2}};  // 2x Minor Health Potion
        by_id_.emplace(q.id, q);
    }
    // Q2 — collect: gather 5 Copper Ore. Turned in to the giver; the player picks
    // ONE of two reward items (choice).
    {
        QuestDef q;
        q.id = Q + 2;
        q.name = "Ore for the Smith";
        q.level = 2;
        q.required_level = 1;
        q.giver_npc_id = kSmith;
        q.objectives = {collect_obj(I + 8, 5)};  // 5x Copper Ore
        q.reward_xp = 40;
        q.reward_money = items::Copper{75};
        q.choice_items = {{I + 1, 1}, {I + 2, 1}};  // Worn Shortsword OR Cracked Buckler
        by_id_.emplace(q.id, q);
    }
    // Q3 — deliver: carry the Tattered Dispatch from the sergeant to the courier.
    // The deliver item is provided to the player on accept (schema note); turn-in
    // is at the courier, NOT the giver.
    {
        QuestDef q;
        q.id = Q + 3;
        q.name = "Deliver the Dispatch";
        q.level = 2;
        q.required_level = 1;
        q.giver_npc_id = kSergeant;
        q.turn_in_npc_id = kCourier;
        q.objectives = {deliver_obj(I + 9, kCourier)};  // Tattered Dispatch -> courier
        q.reward_xp = 30;
        by_id_.emplace(q.id, q);
    }
    // Q4 — explore: scout the ridge. GATED: level 2 AND Q1 completed (a chain edge
    // that QST-02 lint walks). Exercises both the level gate and the prerequisite.
    {
        QuestDef q;
        q.id = Q + 4;
        q.name = "Scout the Ridge";
        q.level = 3;
        q.required_level = 2;
        q.giver_npc_id = kSergeant;
        q.prerequisites = {Q + 1};
        q.objectives = {explore_obj(Z + 1, "kobold_ridge")};
        q.reward_xp = 60;
        q.reward_money = items::Copper{40};
        by_id_.emplace(q.id, q);
    }
}

const QuestDef* PlaceholderQuestStore::find(QuestId quest_id) const {
    auto it = by_id_.find(quest_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<QuestId> PlaceholderQuestStore::ids() const {
    std::vector<QuestId> out;
    out.reserve(by_id_.size());
    for (const auto& [id, _] : by_id_) out.push_back(id);
    std::sort(out.begin(), out.end());  // deterministic (map is unordered)
    return out;
}

}  // namespace meridian::worldd
