// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — the LOOT SESSION: a corpse's server-authoritative loot window
// (ITM-02; server PRD §4-M1 "loot window sessions … loot items into inventory
// with server-side validation"; issue #369).
//
// WHAT THIS IS: the volatile, per-shard state a corpse holds after a creature
// dies and its loot table is rolled (loot_roll.h). It owns the rolled item stacks
// + copper and answers, server-side, "may THIS player take THIS slot right now?"
// — the single validation gate every loot pull passes through. The four checks
// the story requires:
//   * OWNERSHIP    — only an eligible looter (the killer / its threat group at M1)
//                    may loot this corpse (server PRD Pillar 3 "server is law");
//   * IN-RANGE     — the looter must be within loot range of the corpse when it
//                    pulls (an out-of-range pull is rejected, not trusted);
//   * NOT-ALREADY-LOOTED — a common stack (and the money) is taken exactly once;
//                    a second pull of the same slot is rejected (no dupe);
//   * QUEST-GATED  — a quest stack is visible/takeable ONLY by a looter who holds
//                    its required quest (server PRD "quest-item drop rates
//                    conditioned on quest state").
// On a valid pull the stack is transferred into the looter's inventory via the
// meridian::items Inventory API (the single server-authoritative inventory path
// #366 built for loot/vendor/trade to share) — ownership moves from the corpse to
// the player. All-or-nothing: if the inventory has no room the pull throws and the
// session is UNCHANGED (the loot stays on the corpse).
//
// COMMON vs QUEST loot semantics (M1):
//   * A COMMON stack (required_quest_id == 0) and the money are SHARED — the first
//     eligible looter to take them gets them; they then read as looted for everyone.
//   * A QUEST stack is PERSONAL — every eligible looter (a player on that quest)
//     may take their OWN copy exactly once; one looter taking it does not consume
//     it for another. (Group-loot methods — round-robin / need-greed — are GRP-01
//     / M2; personal quest loot is the M1 rule.)
//
// PURE / DB-FREE / SOCKET-FREE: the session is in-memory domain state. The item
// transfer targets a caller-supplied in-memory items::Inventory; minting/placing
// the durable item_instance rows (item_store.h) is the caller's transaction — the
// same mint-then-place seam #366 documents for loot. So the whole module unit-tests
// with no MariaDB. Clean-room, original code (CONTRIBUTING.md).

#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "inventory.h"   // meridian::items::Inventory (the transfer target)
#include "loot_roll.h"   // LootRoll / LootStack

namespace meridian::loot {

// A looter's stable id (an entity guid on the wire). The loot library stays free
// of worldd's Unit model, so it names looters by a plain 64-bit id.
using LooterId = std::uint64_t;

// A 3D point for the range check — the loot library's own tiny vector so it does
// not depend on worldd's Position (movement_validation.h). The map tick converts.
struct LootPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Straight-line distance between two points (the range metric). Header-inline so
// tests + the map tick share one definition.
float loot_distance(const LootPoint& a, const LootPoint& b);

// Default max distance a looter may be from the corpse to pull loot (world units).
// A single per-shard tuning point for M1; real values are content/tuning (mcc #28).
inline constexpr float kDefaultLootRange = 5.0f;

// --- Errors ------------------------------------------------------------------
// Every rejected pull throws exactly one of these; the session (and the target
// inventory) is left UNCHANGED. A worldd handler maps each to a wire loot status.
// Grouped under a common base so a caller may catch broadly.

class LootError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The looter is not an eligible looter of this corpse (ownership).
class NotAnOwner : public LootError {
public:
    NotAnOwner() : LootError("not an eligible looter of this corpse") {}
};

// The looter is too far from the corpse to loot it (in-range).
class LootOutOfRange : public LootError {
public:
    LootOutOfRange() : LootError("looter is out of range of the corpse") {}
};

// The slot index is out of range for this session.
class LootSlotInvalid : public LootError {
public:
    explicit LootSlotInvalid(const std::string& what)
        : LootError("invalid loot slot: " + what) {}
};

// The slot (or the money) has already been looted (no double-loot).
class LootAlreadyTaken : public LootError {
public:
    LootAlreadyTaken() : LootError("that loot has already been taken") {}
};

// The named corpse has no loot session (never dropped, or already cleared). Thrown
// by the map-tick convenience wrapper; a direct LootSession caller never sees it.
class NoSuchCorpse : public LootError {
public:
    NoSuchCorpse() : LootError("no loot session on that corpse") {}
};

// A quest-gated slot pulled by a looter who does not hold the required quest.
class LootQuestRequired : public LootError {
public:
    explicit LootQuestRequired(std::uint32_t quest_id)
        : LootError("that item requires quest " + std::to_string(quest_id)),
          quest_id_(quest_id) {}
    std::uint32_t quest_id() const { return quest_id_; }

private:
    std::uint32_t quest_id_;
};

// A predicate answering "does the looter currently hold quest `quest_id`?" —
// supplied by the caller (worldd's quest state, QST-01) at loot time so quest
// gating always reflects live state, never a stale snapshot.
using QuestPredicate = std::function<bool(std::uint32_t quest_id)>;

// One lootable slot as a looter currently sees it (visible_slots()).
struct LootSlotView {
    std::size_t slot = 0;
    std::uint32_t item_template_id = 0;
    std::uint32_t count = 0;
    std::uint32_t required_quest_id = 0;  // 0 = normal
    bool is_quest() const { return required_quest_id != 0; }
};

// ---------------------------------------------------------------------------
// LootSession — one corpse's loot window.
// ---------------------------------------------------------------------------
class LootSession {
public:
    // `corpse_guid` names the corpse; `corpse_pos` is where it lies (range check);
    // `roll` is the rolled loot (loot_roll.h); `owners` are the eligible looters
    // (the killer / its threat group); `loot_range` is the max pull distance.
    LootSession(LooterId corpse_guid, const LootPoint& corpse_pos, LootRoll roll,
                std::vector<LooterId> owners, float loot_range = kDefaultLootRange);

    // --- queries -------------------------------------------------------------
    LooterId corpse_guid() const { return corpse_guid_; }
    const LootPoint& corpse_pos() const { return corpse_pos_; }
    float loot_range() const { return loot_range_; }
    bool is_owner(LooterId looter) const;

    std::size_t slot_count() const { return roll_.stacks.size(); }
    items::Copper copper() const { return roll_.copper; }
    bool copper_taken() const { return copper_taken_; }

    // Whether `looter` has already taken slot `slot` (a common slot is taken for
    // everyone once anyone takes it; a quest slot is per-looter). Out-of-range
    // slot => false.
    bool slot_taken_by(std::size_t slot, LooterId looter) const;

    // The slots `looter` may currently pull: common slots not yet taken, plus quest
    // slots they qualify for (has_quest true) and have not personally taken. In
    // ascending slot order.
    std::vector<LootSlotView> visible_slots(LooterId looter,
                                            const QuestPredicate& has_quest) const;

    // --- mutations (server-validated) ---------------------------------------

    // Transfer slot `slot` into `inv`. Checks, in order: ownership, in-range,
    // slot valid, quest eligibility, not-already-taken — then inv.add (which may
    // throw items::InventoryFull, leaving the session UNCHANGED). On success marks
    // the slot taken and returns the moved stack. Throws a LootError / an
    // items::InventoryError otherwise.
    LootStack take_item(LooterId looter, const LootPoint& looter_pos, std::size_t slot,
                        const QuestPredicate& has_quest, items::Inventory& inv);

    // Take the corpse's money (shared, once). Checks ownership, in-range, and
    // not-already-taken; returns the copper and marks it taken. Throws a LootError.
    items::Copper take_money(LooterId looter, const LootPoint& looter_pos);

    // The corpse's SHARED loot is exhausted (every common stack taken + the money
    // taken, or there was none) — the map may despawn the corpse. Personal quest
    // stacks do not keep a corpse alive (an ineligible looter never sees them).
    bool fully_looted() const;

private:
    void require_owner_in_range(LooterId looter, const LootPoint& looter_pos) const;

    LooterId corpse_guid_;
    LootPoint corpse_pos_;
    LootRoll roll_;
    std::vector<LooterId> owners_;
    float loot_range_;

    bool copper_taken_ = false;
    // Common (shared) slots taken once: index -> taken. Sized to roll_.stacks.
    std::vector<bool> common_taken_;
    // Quest (personal) slots: which looters have taken slot `i` (i -> guid set).
    std::vector<std::unordered_set<LooterId>> quest_taken_by_;
};

}  // namespace meridian::loot
