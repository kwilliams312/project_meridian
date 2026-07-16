// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — the PURE gossip-menu planner (NPC-01; server PRD §4-M1 "an NPC
// presents a list of options gated by state"; issue #372, epic #20).
//
// WHAT THIS FILE IS: the server-authoritative computation of ONE player's gossip
// menu for ONE NPC. Given the NPC's static roles (npc_def.h) and a read-only VIEW
// of that player's quest state, it returns the ordered list of options the client
// should render — quest options gated by quest state (available / in-progress /
// complete-and-turn-in-able), plus a vendor option and a trainer option when the
// NPC carries those role flags. The CLIENT never decides which options appear;
// this planner is the single source of truth (Principle 1, "server is law").
//
// QUEST-STATE SEAM: the planner needs to know, per quest, whether the player may
// accept it, is on it, or has completed its objectives. Rather than depend on
// worldd's QuestLog directly (which would invert the dependency — worldd is the
// CONSUMER of this library, like it is of meridian::loot), it depends on a tiny
// abstract `QuestStateView`. worldd adapts its QuestLog to this at the call site;
// tests supply a fake. So the npc library stays pure, DB-free, and worldd-free,
// exactly like meridian::loot depends only on meridian::items.
//
// PURE / DB-FREE: no socket, DB, FlatBuffer, RNG, or clock. Runs in the plain
// `server` ctest with no MariaDB.

#ifndef MERIDIAN_NPC_GOSSIP_H
#define MERIDIAN_NPC_GOSSIP_H

#include <cstdint>
#include <vector>

#include "npc_def.h"

namespace meridian::npc {

// The kind of a gossip option — mirrors the wire GossipOptionKind (world.fbs). A
// quest option's `target_id` is the quest id; a vendor/trainer option carries 0.
enum class GossipOptionKind : std::uint8_t {
    kQuestAvailable = 0,   // the player may accept this quest here (a giver + gates pass)
    kQuestInProgress = 1,  // the quest is in the player's log, objectives not all complete
    kQuestComplete = 2,    // objectives complete + this NPC is the turn-in — turn it in here
    kVendor = 3,           // "browse goods" — the NPC's vendor role flag
    kTrainer = 4,          // "train abilities" — the NPC's trainer role flag
};

const char* gossip_option_kind_name(GossipOptionKind k);

// One computed gossip option.
struct GossipOption {
    GossipOptionKind kind = GossipOptionKind::kQuestAvailable;
    std::uint32_t    target_id = 0;  // quest id for quest options; 0 for vendor/trainer
};

// The full menu for one (player, NPC) pair.
struct GossipMenu {
    NpcId                     npc_id = 0;
    std::vector<GossipOption> options;
};

// --- Quest-state view seam ---------------------------------------------------
// A read-only projection of ONE player's quest log, just enough for the gossip
// planner. The consumer (worldd) implements it over its QuestLog; tests fake it.
// Every method is a pure query keyed by quest id.
class QuestStateView {
public:
    virtual ~QuestStateView() = default;

    // The player may ACCEPT `quest_id` right now — not already active, not already
    // completed, and the accept gates (required level + prerequisites) pass. Maps to
    // QuestLog::can_accept(...) == kOk.
    virtual bool can_accept(std::uint32_t quest_id) const = 0;

    // `quest_id` is in the player's log (accepted, not yet turned in).
    virtual bool is_active(std::uint32_t quest_id) const = 0;

    // `quest_id` is active AND every objective is complete (ready to turn in).
    virtual bool is_complete(std::uint32_t quest_id) const = 0;
};

// Compute `player`'s gossip menu for `npc`, given a view of their quest state.
//
// Option order is deterministic (so a resend is byte-stable): quest options first,
// in the NPC's quest-list order, then the vendor option (if a vendor), then the
// trainer option (if a trainer). For each quest the NPC participates in, AT MOST
// ONE quest option is emitted, chosen by the player's state:
//   * complete + this NPC accepts the turn-in            -> kQuestComplete
//   * active (in log, not complete)                      -> kQuestInProgress
//   * can accept + this NPC gives it                     -> kQuestAvailable
//   * otherwise (already done, or gated, or not a giver) -> no option for it
GossipMenu build_gossip_menu(const NpcDef& npc, const QuestStateView& quests);

// --- Overhead quest marker (#844 / #849) -------------------------------------
// The single "floating icon" a client renders over an NPC's head to advertise its
// quest role for THIS player, computed WITHOUT any interaction (unlike the gossip
// menu, which is only built on GOSSIP_HELLO). The classic MMO indicators:
//   * kAvailable         — a `!`: this NPC GIVES a quest the player may accept now.
//   * kTurnInReady        — a lit `?`: the player is on a quest this NPC turns in AND
//                          its objectives are complete (turn it in for the reward).
//   * kTurnInIncomplete   — a greyed `?`: the player is on a quest this NPC turns in
//                          but the objectives are NOT yet complete (a reminder).
//   * kNone               — nothing to advertise.
// Mirrors the wire QuestMarkerKind (world.fbs); the client only DISPLAYS it.
enum class QuestMarker : std::uint8_t {
    kNone = 0,
    kAvailable = 1,          // `!`         — gives && can_accept
    kTurnInReady = 2,        // lit `?`     — turn_in && is_complete
    kTurnInIncomplete = 3,   // greyed `?`  — turn_in && is_active && !is_complete
};

const char* quest_marker_name(QuestMarker m);

// Compute the ONE overhead marker for `npc` as `player` sees it. An NPC may take part
// in several quests, so the per-quest signals are folded into a single marker by a
// fixed PRECEDENCE (most actionable wins), so the icon is deterministic:
//   kTurnInReady  >  kAvailable  >  kTurnInIncomplete  >  kNone
// (a reward ready to collect outranks a new quest to grab, which outranks the
// in-progress reminder). Pure — reuses the SAME QuestStateView seam the gossip
// planner does, so the marker can never disagree with the menu.
QuestMarker compute_quest_marker(const NpcDef& npc, const QuestStateView& quests);

}  // namespace meridian::npc

#endif  // MERIDIAN_NPC_GOSSIP_H
