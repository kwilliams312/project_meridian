// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — the NPC INTERACTION model + the read-only NPC datastore seam
// (NPC-01/02; server PRD §4-M1 "NPC gossip menus … trainers learn abilities for
// copper, gated by class/level"; issue #372, epic #20).
//
// WHAT THIS FILE IS: the static, server-authoritative definition of one
// interactable NPC — the roles it carries (quest giver / turn-in, vendor,
// trainer) and, when it is a trainer, the abilities it teaches (each with a copper
// cost + a class/level gate). This is the data the gossip planner (gossip.h) reads
// to compute a player's menu, and the trainer path (trainer.h) reads to validate a
// learn. It is PURE DATA — no quest state, no player state, no money: those flow in
// at call time.
//
// Provenance / clean-room basis (CONTRIBUTING.md — no GPL/AGPL/CMaNGOS/
// TrinityCore/leaked emulator NPC/gossip/trainer tables consulted):
//   * The NPC role model (an npc_template that can be a quest giver, a vendor, and
//     a trainer, discriminated by flags + child lists) is designed from OUR docs:
//     server PRD §4-M1 (NPC-01 gossip, NPC-02 trainers) and the item/quest module
//     headers (quest_def.h giver/turn_in ids, ability_store.h AbilityId). The
//     "vendor" role is a FLAG only — this library never calls into meridian::vendor
//     (that gossip option is surfaced from the flag; the actual catalog is the
//     vendor story's concern).
//   * The TEMPLATE vs STATE split mirrors quest_def.h / item_template.h: an NpcDef
//     is a STATIC definition shared by every character; a player's interaction
//     (which quest options appear, whether they can afford a train) is computed per
//     call from their live quest log / class / level / balance — never stored here.
//
// DATASTORE SEAM (NPC-01/02, content epic #28 / mcc #28): NPC defs are a read-only
// content artifact, exactly like loot tables (loot_table.h), quest defs
// (quest_def.h) and abilities (ability_store.h). At M1 the real pipeline (mcc #28
// compiles npc.*.yaml → the world DB `npc_template` family) is NOT built. This
// header exposes an ABSTRACT `NpcStore` (the seam, keyed by npc id) plus a
// `PlaceholderNpcStore` (a small ORIGINAL M1 set, npc_def.cpp) so gossip/trainer
// logic has real data to operate on. When mcc #28 lands, a `WorldDbNpcStore`
// implements the SAME seam over the world DB and the placeholder set is dropped —
// no gossip/trainer code changes.
//
// PURE / DB-FREE / SOCKET-FREE: plain data + a small placeholder table. No socket,
// DB, FlatBuffer, RNG, or clock — the npc core runs in the plain `server` ctest
// with no MariaDB (mirrors loot_table.h / quest_def.h / ability_store.h).

#ifndef MERIDIAN_NPC_NPC_DEF_H
#define MERIDIAN_NPC_NPC_DEF_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "item_template.h"  // meridian::items::Copper (money = int64 copper, ECO-01)

namespace meridian::npc {

// A stable content id for an NPC — the world DB `npc_template.id` (IF-9 numeric
// id). 0 is reserved "none".
using NpcId = std::uint32_t;

// One ability a trainer NPC teaches — a reference to an ability (ability_store.h
// AbilityId) plus the copper cost to learn it and the class/level gate. Field set
// mirrors the future world-DB `npc_trainer` shape (server SAD §4.4). The cost is
// SERVER content — the client never supplies or computes it (Principle 1).
struct TrainerAbility {
    std::uint32_t ability_id = 0;      // -> meridian::worldd::AbilityStore (IF-9)
    items::Copper cost = 0;            // copper to learn (>= 0, ECO-01, server-computed)
    std::uint8_t  required_class = 0;  // roster.h Class id gate (0 = any class may learn)
    std::uint16_t required_level = 1;  // minimum character level to learn (>= 1)
};

// A quest an NPC participates in, for the gossip menu's quest options. `gives` =
// this NPC offers the quest (a giver); `turn_in` = this NPC accepts its turn-in.
// Both may be true (giver == turn-in NPC, the common case). Mirrors quest_def.h's
// giver_npc_id / turn_in_npc(); the gossip planner cross-references the player's
// quest log against these to decide which quest option (if any) to surface.
struct NpcQuestRef {
    std::uint32_t quest_id = 0;
    bool gives = false;
    bool turn_in = false;
};

// A STATIC NPC definition (NPC-01/02). One per distinct NPC; shared by every
// character. Read-only at runtime — produced by the NPC datastore (placeholder set
// now, mcc #28 later). Field set mirrors the future npc.schema.yaml / world DB
// `npc_template` (+ `npc_quest`, `npc_trainer`) family.
struct NpcDef {
    NpcId       id = 0;      // npc_template.id (IF-9 numeric id)
    std::string name;        // npc_template.name (displayName)

    // Role flags. `is_vendor` is a FLAG ONLY — it surfaces the "browse goods"
    // gossip option; this library never calls into meridian::vendor (the catalog
    // is the vendor story's concern). `is_trainer` gates the trainer path + the
    // "train" gossip option; trainer_abilities is meaningful only when set.
    bool is_vendor = false;
    bool is_trainer = false;

    std::vector<NpcQuestRef>    quests;             // quest giver / turn-in participation
    std::vector<TrainerAbility> trainer_abilities;  // taught abilities (trainer only)

    // The trainer entry teaching `ability_id`, or nullptr if this NPC does not
    // teach it. Linear over a handful of entries (an NPC teaches few abilities).
    const TrainerAbility* trainer_ability(std::uint32_t ability_id) const {
        for (const auto& t : trainer_abilities)
            if (t.ability_id == ability_id) return &t;
        return nullptr;
    }
};

// --- NPC datastore seam (NPC-01/02, content epic #28) ------------------------
// The read-only source of NPC defs. The gossip planner + trainer path depend ONLY
// on this interface, never on where defs come from. M1 wires the placeholder
// implementation below; mcc #28 later adds a world-DB implementation of the SAME
// interface and the placeholder set is deleted — no consumer changes.
class NpcStore {
public:
    virtual ~NpcStore() = default;

    // The def for `npc_id`, or nullptr if unknown. The returned pointer is owned by
    // the store and valid for the store's lifetime.
    virtual const NpcDef* find(NpcId npc_id) const = 0;

    // Every known npc id, ascending (tests / dev tooling).
    virtual std::vector<NpcId> ids() const = 0;
};

// Reserved id range for the M1 PLACEHOLDER NPC set. Real content ids (mcc #28) are
// authored in content and will not collide with this dev-only range — keeping
// placeholder ids distinct means a stray placeholder never masquerades as a real
// compiled NPC once #28 lands. Chosen to sit alongside the quest module's
// placeholder NPC band (quest_def.h kPlaceholderNpcIdBase = 700000) so the
// placeholder giver/turn-in NPCs the gossip chain references line up.
inline constexpr NpcId kPlaceholderNpcIdBase = 700000;

// Named placeholder NPCs the M1 gossip/trainer set defines. Distinct so tests can
// address the exact archetype they want:
//   * kNpcQuestGiver — a plain quest giver/turn-in (gossip quest options only).
//   * kNpcTrainer    — a class trainer that also gives a quest (trainer + quest
//                      options; teaches a class-gated and an any-class ability).
//   * kNpcVendor     — a vendor (the vendor gossip FLAG option only).
inline constexpr NpcId kNpcQuestGiver = kPlaceholderNpcIdBase + 1;
inline constexpr NpcId kNpcTrainer    = kPlaceholderNpcIdBase + 2;
inline constexpr NpcId kNpcVendor     = kPlaceholderNpcIdBase + 3;

// The placeholder quest the M1 NPCs give/turn-in (a stand-in quest id; quests are
// QST-01 / mcc #28). Matches the quest module's placeholder band so a worldd wiring
// that installs both stores cross-references the same id.
inline constexpr std::uint32_t kPlaceholderQuestId = 800001;

// The M1 placeholder ability ids a trainer teaches. These reference the CMB-01
// placeholder ability band (ability_store.h kPlaceholderIdBand = 0xF0000000);
// mirrored here as plain constants so this library does not depend on worldd.
//   * kTrainedStrike — a class-gated ability (Vanguard only; the melee strike).
//   * kTrainedHeal   — an any-class ability (the heal), still level-gated.
inline constexpr std::uint32_t kPlaceholderAbilityBand = 0xF0000000u;
inline constexpr std::uint32_t kTrainedStrike = kPlaceholderAbilityBand + 1;  // Vanguard-only
inline constexpr std::uint32_t kTrainedHeal   = kPlaceholderAbilityBand + 3;  // any class

// roster.h Class id for kVanguard (the melee class kTrainedStrike is gated to).
// Mirrored as a plain constant so this library does not depend on meridian-
// characters; the value is the M0-frozen roster id (roster.h Class::kVanguard = 1).
inline constexpr std::uint8_t kClassVanguard = 1;

// The M1 placeholder NPC set (NPC-01/02). A small ORIGINAL, clean-room set
// (npc_def.cpp) that exercises every gossip/trainer axis the planner + trainer
// path must handle: a quest giver, a trainer (with a class-gated + an any-class
// ability, both level-gated + priced), and a vendor flag. NOT the content pipeline
// — the seam's stand-in, dropped when mcc #28 lands.
class PlaceholderNpcStore : public NpcStore {
public:
    PlaceholderNpcStore();
    const NpcDef* find(NpcId npc_id) const override;
    std::vector<NpcId> ids() const override;

private:
    std::unordered_map<NpcId, NpcDef> by_id_;
};

}  // namespace meridian::npc

#endif  // MERIDIAN_NPC_NPC_DEF_H
