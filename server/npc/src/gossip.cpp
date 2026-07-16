// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — the pure gossip-menu planner (NPC-01; issue #372).
// See gossip.h for the seam rationale. Clean-room, original (CONTRIBUTING.md).

#include "gossip.h"

namespace meridian::npc {

const char* gossip_option_kind_name(GossipOptionKind k) {
    switch (k) {
        case GossipOptionKind::kQuestAvailable:  return "quest_available";
        case GossipOptionKind::kQuestInProgress: return "quest_in_progress";
        case GossipOptionKind::kQuestComplete:   return "quest_complete";
        case GossipOptionKind::kVendor:          return "vendor";
        case GossipOptionKind::kTrainer:         return "trainer";
    }
    return "unknown";
}

GossipMenu build_gossip_menu(const NpcDef& npc, const QuestStateView& quests) {
    GossipMenu menu;
    menu.npc_id = npc.id;

    // Quest options first, in the NPC's quest-list order. At most one option per
    // quest, chosen by the player's state (see the header's precedence contract):
    // complete-turn-in > in-progress > available. States that surface nothing
    // (already turned in, gated, or this NPC neither gives nor takes it) emit no
    // option — the client only ever sees actionable quest options.
    for (const NpcQuestRef& q : npc.quests) {
        if (q.turn_in && quests.is_complete(q.quest_id)) {
            menu.options.push_back(GossipOption{GossipOptionKind::kQuestComplete, q.quest_id});
        } else if (quests.is_active(q.quest_id)) {
            // Active but not complete — surfaced as in-progress regardless of which
            // role (giver or turn-in) this NPC plays, so the player can check status.
            menu.options.push_back(GossipOption{GossipOptionKind::kQuestInProgress, q.quest_id});
        } else if (q.gives && quests.can_accept(q.quest_id)) {
            menu.options.push_back(GossipOption{GossipOptionKind::kQuestAvailable, q.quest_id});
        }
    }

    // Role-flag options, deterministic order: vendor then trainer.
    if (npc.is_vendor) {
        menu.options.push_back(GossipOption{GossipOptionKind::kVendor, 0});
    }
    if (npc.is_trainer) {
        menu.options.push_back(GossipOption{GossipOptionKind::kTrainer, 0});
    }

    return menu;
}

const char* quest_marker_name(QuestMarker m) {
    switch (m) {
        case QuestMarker::kNone:              return "none";
        case QuestMarker::kAvailable:         return "available";
        case QuestMarker::kTurnInReady:       return "turn_in_ready";
        case QuestMarker::kTurnInIncomplete:  return "turn_in_incomplete";
    }
    return "unknown";
}

QuestMarker compute_quest_marker(const NpcDef& npc, const QuestStateView& quests) {
    // Fold every quest this NPC participates in into ONE marker by precedence
    // (see the header): a ready turn-in outranks an available quest, which outranks
    // an in-progress turn-in reminder. Track the best seen and stop early once the
    // top rank is reached.
    QuestMarker best = QuestMarker::kNone;
    auto rank = [](QuestMarker m) -> int {
        switch (m) {
            case QuestMarker::kTurnInReady:      return 3;
            case QuestMarker::kAvailable:        return 2;
            case QuestMarker::kTurnInIncomplete: return 1;
            case QuestMarker::kNone:             return 0;
        }
        return 0;
    };
    for (const NpcQuestRef& q : npc.quests) {
        QuestMarker here = QuestMarker::kNone;
        if (q.turn_in && quests.is_complete(q.quest_id)) {
            here = QuestMarker::kTurnInReady;
        } else if (q.gives && quests.can_accept(q.quest_id)) {
            here = QuestMarker::kAvailable;
        } else if (q.turn_in && quests.is_active(q.quest_id)) {
            here = QuestMarker::kTurnInIncomplete;
        }
        if (rank(here) > rank(best)) best = here;
        if (best == QuestMarker::kTurnInReady) break;  // top rank — cannot improve
    }
    return best;
}

}  // namespace meridian::npc
