// SPDX-License-Identifier: Apache-2.0
//
// worldd — the per-character QUEST STATE MACHINE (issue #371, QST-01; the spine
// of the M1 game loop, epic #20). Server-authoritative, from OUR PRD/SAD (server
// PRD §4-M1 QST-01) — accept from a giver (validate level + prerequisites), track
// objectives, validate turn-in at the turn-in NPC, and compute the reward bundle.
//
//   accept ─► in-progress ─► (all objectives complete) ─► turn-in ─► completed
//     │ gate: required_level + prerequisites
//     └─ objectives advance from four SOURCES, each a typed event sink:
//          kill    ── on_kill(npc)           ◄ map tick on_unit_died  (#359/#365)
//          collect ── sync_collect(inventory)◄ inventory counts       (#366)
//          deliver ── on_deliver(npc, item)  ◄ turn-in-at-NPC interact
//          explore ── on_explore(zone, poi)  ◄ area trigger OnAreaTrigger (#368)
//
// REWARD GRANT (turn_in): item rewards are minted into the character's inventory
// (#366) here, all-or-nothing on backpack room; XP and copper are returned as
// AMOUNTS for the caller to apply via the leveling (#360, leveling.h grant_xp)
// and economy (ECO-01, currency.h add_money) paths. The quest core itself is
// DB-FREE and never persists — it is the pure domain logic, exactly like
// leveling.h / area_triggers.h / inventory.h, so it runs in the plain `server`
// ctest with no MariaDB and every transition is deterministic and reproducible
// (leveling/quests must be server-authoritative and byte-stable in the golden
// sim harness).
//
// DEFS SEAM: the state machine reads QuestDef from an abstract QuestStore
// (quest_def.h) — the M1 PlaceholderQuestStore now, a world-DB store via mcc #28
// later, with NO change here (the content seam this whole epic leaves open).
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md (QST-01), quest.schema.yaml /
// schema/sql/world/40_quest.sql (the quest model), and the #359/#366/#368/#360
// module headers ONLY. No GPL / AGPL / CMaNGOS / TrinityCore / leaked emulator
// quest logic consulted — every gate, transition and reward rule is ORIGINAL,
// derived from OUR schema. See CONTRIBUTING.md.

#ifndef MERIDIAN_WORLDD_QUEST_LOG_H
#define MERIDIAN_WORLDD_QUEST_LOG_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "inventory.h"   // meridian::items::Inventory / ItemInstance (reward grant, collect)
#include "quest_def.h"   // QuestStore / QuestDef / QuestObjective / QuestRewardItem

namespace meridian::worldd {

// Typed outcome of an accept attempt (an offer from a giver NPC). The state
// machine gates on the quest's required_level and its prerequisite quests.
enum class AcceptStatus : std::uint8_t {
    kOk = 0,                  // accepted — the quest is now in progress
    kUnknownQuest,            // no such quest in the store
    kAlreadyActive,           // already in the character's log
    kAlreadyCompleted,        // already turned in (not repeatable at M1)
    kLevelTooLow,             // character level below required_level
    kMissingPrerequisite,     // a prerequisite quest is not yet completed
};

const char* accept_status_name(AcceptStatus s);

// Typed outcome of a turn-in attempt at an NPC. Turn-in validates the quest is
// active, complete, at the correct NPC, the reward choice is legal, and there is
// backpack room for the reward items (all-or-nothing).
enum class TurnInStatus : std::uint8_t {
    kOk = 0,          // turned in — rewards granted, quest completed
    kUnknownQuest,    // no such quest in the store
    kNotActive,       // not in the character's log (never accepted / already done)
    kWrongNpc,        // this NPC is not the quest's turn-in NPC
    kIncomplete,      // one or more objectives not yet complete
    kBadChoice,       // choice_index missing/out of range for a choice-reward quest
    kInventoryFull,   // no backpack room for the reward items (nothing changed)
};

const char* turn_in_status_name(TurnInStatus s);

// Live progress of ONE objective: units accumulated toward the goal.
struct ObjectiveState {
    std::uint16_t have = 0;
    std::uint16_t need = 1;
    bool complete() const { return have >= need; }
};

// The reward bundle produced by a successful turn_in(). The reward ITEMS have
// ALREADY been minted into the inventory passed to turn_in(); `items` echoes them
// for the S->C reply. `xp` and `money` are AMOUNTS the caller applies via the
// leveling / economy paths (the quest core is DB-free — it never persists money
// or XP itself).
struct RewardGrant {
    std::uint32_t                xp = 0;
    items::Copper                money = 0;
    std::vector<QuestRewardItem> items;  // always-granted items + the chosen choice item
};

// ---------------------------------------------------------------------------
// QuestLog — one character's quest state machine.
// ---------------------------------------------------------------------------
//
// Owned by the world thread (single-threaded game state, SAD §2.5/§6) alongside
// the character's session. Not thread-safe by itself — the world-thread ownership
// serializes access, exactly like AoiGrid / AreaTriggerSet. `store` is borrowed
// and must outlive the log (the boot-loaded quest datastore).
class QuestLog {
public:
    explicit QuestLog(const QuestStore& store) : store_(&store) {}

    // --- queries -------------------------------------------------------------
    bool is_active(QuestId id) const { return active_.count(id) != 0; }
    bool is_completed(QuestId id) const { return completed_.count(id) != 0; }

    // Whether objective `index` of an ACTIVE quest is complete (false if the quest
    // is not active or the index is out of range).
    bool is_objective_complete(QuestId id, std::size_t index) const;

    // Whether EVERY objective of an active quest is complete (ready to turn in).
    // False if the quest is not active.
    bool is_complete(QuestId id) const;

    // The live objective states of an active quest, or nullptr if not active.
    const std::vector<ObjectiveState>* objectives(QuestId id) const;

    // Active / completed quest ids, ascending (deterministic).
    std::vector<QuestId> active_quests() const;
    std::vector<QuestId> completed_quests() const;

    // --- accept (from a giver) ----------------------------------------------
    // Whether the character may accept `id` at `player_level` (the gate check
    // without mutating). See AcceptStatus.
    AcceptStatus can_accept(QuestId id, std::uint16_t player_level) const;

    // Accept `id`: on kOk the quest enters the log with all objectives at zero.
    // (For a deliver objective the deliver item is provided on accept — minting it
    // into the inventory is the caller's concern, per quest.schema.yaml.) Returns
    // the same status can_accept() would; the log is unchanged unless kOk.
    AcceptStatus accept(QuestId id, std::uint16_t player_level);

    // --- objective progress (the four sources) ------------------------------
    // Each advances every matching objective of every active quest and returns
    // true iff any objective's `have` changed (so the caller emits QUEST_PROGRESS).

    // A creature of template `npc_template_id` died to this character (map tick
    // on_unit_died, #359/#365). Advances kill objectives (capped at the goal).
    bool on_kill(std::uint32_t npc_template_id);

    // The character discovered POI (`zone_id`, `poi`) via an area trigger (#368).
    // Completes matching explore objectives.
    bool on_explore(std::uint32_t zone_id, const std::string& poi);

    // The character delivered `item_id` to NPC `to_npc_id`. Completes matching
    // deliver objectives.
    bool on_deliver(std::uint32_t to_npc_id, std::uint32_t item_id);

    // Recompute every active collect objective's `have` from the character's
    // current inventory contents (min(units held, goal), #366). Call before a
    // turn-in and whenever the inventory changes for a collect quest.
    bool sync_collect(const items::Inventory& inv);

    // --- turn-in (at the turn-in NPC) ---------------------------------------
    // Whether `id` may be turned in at NPC `npc_id` with reward choice
    // `choice_index` (>= 0 selects rewards.choice_items[]; -1 = no choice). Does
    // NOT check inventory room (that is turn_in's all-or-nothing responsibility);
    // callers wanting the collect state fresh should sync_collect() first.
    TurnInStatus can_turn_in(QuestId id, std::uint32_t npc_id, int choice_index) const;

    // Turn `id` in at NPC `npc_id`. Re-syncs collect objectives from `inv`,
    // validates (active / complete / correct NPC / legal choice), checks backpack
    // room, mints the reward items into `inv`, marks the quest completed, and fills
    // `out` with the reward bundle. On any non-kOk status NOTHING is changed
    // (all-or-nothing). `choice_index` selects rewards.choice_items[] for a choice
    // quest (>= 0) or is -1 when the quest has no choice rewards.
    TurnInStatus turn_in(QuestId id, std::uint32_t npc_id, items::Inventory& inv,
                         int choice_index, RewardGrant& out);

private:
    // The def for `id`, or nullptr (unknown quest / defensive).
    const QuestDef* def(QuestId id) const { return store_->find(id); }

    const QuestStore* store_;

    // An active quest: the live per-objective progress (index-aligned with the
    // def's objectives). std::map keeps active-quest iteration deterministic.
    struct Entry {
        std::vector<ObjectiveState> objectives;
    };
    std::map<QuestId, Entry> active_;
    std::set<QuestId>        completed_;  // turned-in quest ids (prerequisite source)
};

// Count the units of item template `template_id` the character currently holds
// (backpack + equipped). Shared by sync_collect and reusable by callers building
// a collect-objective view. Pure over the inventory snapshot.
std::uint32_t count_items(const items::Inventory& inv, std::uint32_t template_id);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_QUEST_LOG_H
