// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — the M1 PLACEHOLDER NPC set (NPC-01/02; issue #372).
// See npc_def.h for the datastore-seam rationale (mcc #28 replaces this).
//
// These NPCs are ORIGINAL, clean-room placeholders (CONTRIBUTING.md — no
// GPL/AGPL/CMaNGOS/TrinityCore/leaked NPC/gossip/trainer tables consulted). They
// reference the placeholder quest (quest_def.cpp band) and the placeholder ability
// band (ability_store.h). Together they cover every gossip/trainer axis the
// planner + trainer path must exercise:
//   * a quest giver / turn-in (quest gossip options — available/in-progress/complete),
//   * a trainer with a CLASS-GATED ability (Vanguard-only) and an ANY-CLASS ability,
//     both LEVEL-GATED and PRICED in copper (the four trainer rejection axes),
//   * a vendor (the vendor gossip role FLAG — no call into meridian::vendor).

#include "npc_def.h"

namespace meridian::npc {

PlaceholderNpcStore::PlaceholderNpcStore() {
    // --- kNpcQuestGiver: a plain quest giver / turn-in ------------------------
    // Gives and accepts the placeholder quest, so the gossip planner surfaces the
    // available → in-progress → complete option as the player's state advances.
    {
        NpcDef n;
        n.id = kNpcQuestGiver;
        n.name = "Placeholder Quest Giver";
        n.quests.push_back(NpcQuestRef{kPlaceholderQuestId, /*gives=*/true, /*turn_in=*/true});
        by_id_.emplace(n.id, std::move(n));
    }

    // --- kNpcTrainer: a class trainer that also gives a quest ------------------
    // Teaches two abilities: a Vanguard-only strike (level 2, 50 copper) and an
    // any-class heal (level 5, 120 copper). Also gives the placeholder quest so the
    // menu can carry BOTH a trainer option and a quest option at once.
    {
        NpcDef n;
        n.id = kNpcTrainer;
        n.name = "Placeholder Trainer";
        n.is_trainer = true;
        n.trainer_abilities.push_back(TrainerAbility{
            kTrainedStrike, /*cost=*/50, /*required_class=*/kClassVanguard, /*required_level=*/2});
        n.trainer_abilities.push_back(TrainerAbility{
            kTrainedHeal, /*cost=*/120, /*required_class=*/0 /*any class*/, /*required_level=*/5});
        n.quests.push_back(NpcQuestRef{kPlaceholderQuestId, /*gives=*/true, /*turn_in=*/true});
        by_id_.emplace(n.id, std::move(n));
    }

    // --- kNpcVendor: a vendor (role FLAG only) --------------------------------
    // Carries the vendor role flag so the gossip menu surfaces a "browse goods"
    // option. This library never calls into meridian::vendor — the flag is all the
    // gossip contract needs; the catalog is the vendor story's concern.
    {
        NpcDef n;
        n.id = kNpcVendor;
        n.name = "Placeholder Vendor";
        n.is_vendor = true;
        // Sells from the M1 placeholder general-goods catalog. Literal to avoid a
        // dependency on meridian::vendor — mirrors vendor_catalog.h
        // kPlaceholderGeneralVendor (kPlaceholderVendorIdBase 990000 + 1). When mcc
        // #28 compiles real NPCs this id comes from npc content (#453).
        n.vendor_id = 990001u;
        by_id_.emplace(n.id, std::move(n));
    }
}

const NpcDef* PlaceholderNpcStore::find(NpcId npc_id) const {
    auto it = by_id_.find(npc_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<NpcId> PlaceholderNpcStore::ids() const {
    std::vector<NpcId> out;
    out.reserve(by_id_.size());
    for (const auto& [id, _] : by_id_) out.push_back(id);
    // Ascending so callers/tests get a deterministic order (the map is unordered).
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        for (std::size_t j = i + 1; j < out.size(); ++j)
            if (out[j] < out[i]) std::swap(out[i], out[j]);
    return out;
}

}  // namespace meridian::npc
