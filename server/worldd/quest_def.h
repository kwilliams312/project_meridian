// SPDX-License-Identifier: Apache-2.0
//
// worldd — quest DEFINITIONS + the read-only quest datastore seam (issue #371,
// QST-01; server PRD §4-M1, epic #20). The STATIC quest content the quest state
// machine (quest_log.h) reads: a giver, level/prerequisite gates, an ordered
// objective list (kill / collect / deliver / explore), and a reward bundle
// (XP + copper + always-granted items + a one-of choice).
//
// Provenance / clean-room basis (CONTRIBUTING.md — no GPL/AGPL/CMaNGOS/
// TrinityCore/leaked source consulted):
//   * The QuestDef FIELD SET mirrors OUR content schema (schema/content/
//     quest.schema.yaml, `meridian/quest@1`) and its compiled world-DB shape
//     (schema/sql/world/40_quest.sql `quest_template` / `quest_objective` /
//     `quest_prereq` / `quest_reward`). Those files are the authoritative quest
//     model; this struct is the server-side in-memory view of one compiled row
//     set. Every objective/reward field maps 1:1 to a documented column.
//   * The TEMPLATE vs STATE split mirrors the item model (item_template.h vs
//     inventory.h): a QuestDef is a STATIC definition shared by every character;
//     a character's PROGRESS through it lives in quest_log.h. Defs never change
//     at runtime.
//
// DATASTORE SEAM (QST-01, content epic #28): quest defs are a read-only artifact.
// At M1 the real quest pipeline (mcc #28 compiles quest.schema.yaml -> the world
// DB `quest_template` tables, replaced wholesale nightly) is NOT built. This
// header exposes an ABSTRACT `QuestStore` (the seam) plus a
// `PlaceholderQuestStore` (a small original M1 chain, quest_def.cpp). When mcc
// #28 lands, a `WorldDbQuestStore` implements the SAME seam over the world DB and
// the placeholder set is dropped — no quest_log / dispatch code changes.
//
// PURE / DB-FREE / SOCKET-FREE: plain data + a small placeholder table. No
// socket, DB, FlatBuffer, RNG, or clock — the quest core runs in the plain
// `server` ctest with no MariaDB (mirrors leveling.h / area_triggers.h /
// item_template.h).

#ifndef MERIDIAN_WORLDD_QUEST_DEF_H
#define MERIDIAN_WORLDD_QUEST_DEF_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "item_template.h"  // meridian::items::Copper (money = int64 copper, ECO-01)

namespace meridian::worldd {

// A stable content id for a quest — the world DB `quest_template.id` (IF-9
// numeric id). 0 is reserved "none".
using QuestId = std::uint32_t;

// The kind of an objective — mirrors quest_objective.type
// ENUM('kill','collect','deliver','explore') (40_quest.sql), discriminated by
// which fields of QuestObjective are meaningful.
enum class ObjectiveType : std::uint8_t {
    kKill = 0,     // kill `count` creatures of `target_npc_id` (via on_unit_died)
    kCollect = 1,  // hold `count` of `item_id` in the inventory (via inventory)
    kDeliver = 2,  // deliver `item_id` (granted on accept) to `to_npc_id`
    kExplore = 3,  // enter POI (`zone_id`, `poi`) via an area trigger
};

const char* objective_type_name(ObjectiveType t);

// One quest objective (a quest_objective row). Which fields matter depends on
// `type` (the oneOf in quest.schema.yaml):
//   kKill:    target_npc_id + count
//   kCollect: item_id + count
//   kDeliver: item_id + to_npc_id            (count is implicitly 1)
//   kExplore: zone_id + poi                  (count is implicitly 1)
struct QuestObjective {
    ObjectiveType type = ObjectiveType::kKill;
    std::uint32_t target_npc_id = 0;  // kill.target (npcRef)
    std::uint32_t item_id = 0;        // collect.item / deliver.item (itemRef)
    std::uint32_t to_npc_id = 0;      // deliver.to (npcRef)
    std::uint32_t zone_id = 0;        // explore.zone (zoneRef)
    std::string   poi;                // explore.poi (zone-local string id)
    std::uint16_t count = 1;          // kill/collect goal (1..200); 1 for deliver/explore

    // The number of "units" that complete this objective (kill/collect use
    // `count`; deliver/explore are binary — a single unit).
    std::uint16_t required() const {
        return (type == ObjectiveType::kKill || type == ObjectiveType::kCollect)
                   ? count
                   : std::uint16_t{1};
    }
};

// A reward item (a quest_reward row). `count` units of `item_id` are minted to
// the turning-in character. `is_choice` distinguishes the two schema arrays:
// rewards.items[] (always granted) from rewards.choice_items[] (pick exactly one).
struct QuestRewardItem {
    std::uint32_t item_id = 0;
    std::uint16_t count = 1;
};

// A STATIC quest definition (QST-01). One per distinct quest; shared by every
// character. Read-only at runtime — produced by the quest datastore (placeholder
// set now, mcc #28 later). Field set mirrors quest.schema.yaml / 40_quest.sql.
struct QuestDef {
    QuestId       id = 0;               // quest_template.id (IF-9 numeric id)
    std::string   name;                 // quest_template.name (displayName)
    std::uint16_t level = 1;            // quest_template.level (quest level)
    std::uint16_t required_level = 1;   // quest_template.required_level (min to accept)
    std::uint32_t giver_npc_id = 0;     // quest_template.giver_npc_id (offer NPC)
    std::uint32_t turn_in_npc_id = 0;   // quest_template.turn_in_npc_id (0 = giver)

    std::vector<QuestId>        prerequisites;  // quest_prereq.prereq_quest_id (ALL required)
    std::vector<QuestObjective> objectives;     // quest_objective (1..4, ordered)

    std::uint32_t                reward_xp = 0;     // quest_template.reward_xp
    items::Copper                reward_money = 0;  // quest_template.reward_money (copper)
    std::vector<QuestRewardItem> reward_items;      // rewards.items[]  (always granted)
    std::vector<QuestRewardItem> choice_items;      // rewards.choice_items[] (pick one)

    // The NPC a completed quest is turned in at — the explicit turn_in NPC, or
    // the giver when unset (quest.schema.yaml: "turn_in defaults to giver").
    std::uint32_t turn_in_npc() const {
        return turn_in_npc_id != 0 ? turn_in_npc_id : giver_npc_id;
    }
};

// --- Quest datastore seam (QST-01, content epic #28) -------------------------
// The read-only source of quest defs. The quest state machine + dispatch depend
// ONLY on this interface, never on where defs come from. M1 wires the placeholder
// implementation below; mcc #28 later adds a world-DB implementation of the SAME
// interface and the placeholder chain is deleted — no consumer changes.
class QuestStore {
public:
    virtual ~QuestStore() = default;

    // The def for `quest_id`, or nullptr if unknown. The returned pointer is owned
    // by the store and valid for the store's lifetime.
    virtual const QuestDef* find(QuestId quest_id) const = 0;

    // Every known quest id, ascending (tests / dev tooling).
    virtual std::vector<QuestId> ids() const = 0;
};

// Reserved id range for the M1 PLACEHOLDER quest set. Real content ids (mcc #28)
// are authored in content and will not collide with this dev-only range — keeping
// placeholder ids distinct means a stray placeholder never masquerades as a real
// compiled quest once #28 lands.
inline constexpr QuestId kPlaceholderQuestIdBase = 800000;

// Placeholder NPC / zone ids used by the M1 quest chain. Distinct high ranges so
// they never collide with real compiled content (mcc #28) — the giver/turn-in
// NPCs and the explore zone are dev-only stand-ins until real world data lands.
inline constexpr std::uint32_t kPlaceholderNpcIdBase = 700000;
inline constexpr std::uint32_t kPlaceholderZoneIdBase = 500000;

// The M1 placeholder quest chain (QST-01). A small ORIGINAL, clean-room set that
// exercises every objective type (kill / collect / deliver / explore), a level
// gate, and a prerequisite edge — just enough for the state machine, dispatch and
// tests to operate on real data before mcc #28 produces the content-authored
// 10-quest chain (#28). NOT the content pipeline: it is the seam's stand-in
// implementation, dropped when #28 lands. Reward/objective item ids reference the
// placeholder item templates (item_template.cpp, kPlaceholderIdBase).
class PlaceholderQuestStore : public QuestStore {
public:
    PlaceholderQuestStore();
    const QuestDef* find(QuestId quest_id) const override;
    std::vector<QuestId> ids() const override;

private:
    std::unordered_map<QuestId, QuestDef> by_id_;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_QUEST_DEF_H
