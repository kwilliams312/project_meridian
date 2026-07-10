// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — the TRAINER path: pure learn-eligibility plan + the DB apply that
// debits copper and grants the ability (NPC-02; server PRD §4-M1 "trainers: learn
// an ability for copper, gated by class + level"; issue #372, epic #20).
//
// Two layers, mirroring meridian::items/currency.h (pure invariant core + a single
// transactional DB op) and the loot/quest split (pure plan, then apply):
//   1. PURE plan (plan_learn) — the invariant core: given the NPC's trainer entry,
//      the player's class/level, their current copper balance, and whether they
//      already know the ability, decide the TYPED outcome (ok / wrong-class /
//      too-low-level / already-known / insufficient-funds / not-a-trainer /
//      unknown-ability). No DB, no mutation — deterministically unit-tested.
//   2. DB apply (learn_ability) — on a plan of kOk ONLY, debit the cost from
//      `character.money` through currency::subtract_money (ONE row-locked
//      characters-DB transaction, ECO-01) and record the ability as learned.
//      All-or-nothing: on any rejection NOTHING changes (money untouched).
//
// LEARNED ABILITIES (M1 scope): the set of abilities a character has learned is
// held IN MEMORY for M1 (LearnedAbilitySet) — there is no `character_ability`
// durable table yet (that persistence is a later story / mcc #28 schema work). The
// COPPER debit IS durable (it moves real `character.money`); the learned-ability
// row is not. The DB integration test therefore asserts the copper debit through
// the real DB and treats the learned set as in-memory (documented there).
//
// CLEAN-ROOM: designed from OUR docs (server PRD §4-M1 NPC-02, currency.h,
// ability_store.h, npc_def.h) — no GPL/AGPL/CMaNGOS/TrinityCore/leaked emulator
// trainer logic consulted. See CONTRIBUTING.md.

#ifndef MERIDIAN_NPC_TRAINER_H
#define MERIDIAN_NPC_TRAINER_H

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "item_template.h"            // meridian::items::Copper
#include "meridian/db/connection.h"   // db::Connection (DB apply path)
#include "npc_def.h"

namespace meridian::npc {

// Typed outcome of a learn attempt (pure plan + DB apply share it). Mirrors the
// wire TrainerLearnStatus (world.fbs) 1:1.
enum class TrainStatus : std::uint8_t {
    kOk = 0,                // may learn (plan) / learned + copper debited (apply)
    kNotTrainer,           // the NPC is not a trainer
    kUnknownAbility,       // the NPC does not teach this ability id
    kWrongClass,           // the character's class is not the ability's required class
    kLevelTooLow,          // the character's level is below the required level
    kAlreadyKnown,         // the character already knows this ability
    kInsufficientFunds,    // the character cannot afford the cost
};

const char* train_status_name(TrainStatus s);

// The result of PLANNING a learn (pure, no side effects). On kOk, `cost` is the
// copper to debit and `ability_id` echoes the request; on any rejection they still
// echo the request/cost for the caller's reply, but nothing is (or should be) moved.
struct TrainPlan {
    TrainStatus   status = TrainStatus::kOk;
    std::uint32_t ability_id = 0;
    items::Copper cost = 0;
};

// PURE: decide whether a character may learn `ability_id` from `npc`. Checks, in a
// fixed order (so the reported reason is deterministic):
//   1. NPC is a trainer                     -> else kNotTrainer
//   2. NPC teaches this ability              -> else kUnknownAbility
//   3. already known                         -> kAlreadyKnown
//   4. right class (0 = any)                 -> else kWrongClass
//   5. high enough level                     -> else kLevelTooLow
//   6. can afford the cost                   -> else kInsufficientFunds
// `known` = does the character already know the ability. `balance` = their current
// copper. No DB, no mutation.
TrainPlan plan_learn(const NpcDef& npc, std::uint32_t ability_id,
                     std::uint8_t char_class, std::uint16_t char_level,
                     items::Copper balance, bool known);

// The in-memory set of abilities one character has learned (M1 — see file header).
// A thin wrapper over a set so the trainer path has a single "knows / learn" API
// the future durable `character_ability` store can replace behind the same shape.
class LearnedAbilitySet {
public:
    bool knows(std::uint32_t ability_id) const { return known_.count(ability_id) != 0; }
    // Record `ability_id` as learned. Returns true if newly added (false if it was
    // already present — a defensive no-op; the trainer path rejects a re-learn before
    // reaching here).
    bool learn(std::uint32_t ability_id) { return known_.insert(ability_id).second; }
    std::size_t size() const { return known_.size(); }

    // The learned ability ids (M1 in-memory set). Order is UNSPECIFIED (a hash set);
    // a caller that needs a stable wire order sorts. Used by the KNOWN_ABILITIES
    // projection (world.fbs, #457) to build the character's spellbook push.
    std::vector<std::uint32_t> ids() const {
        return std::vector<std::uint32_t>(known_.begin(), known_.end());
    }

private:
    std::unordered_set<std::uint32_t> known_;
};

// The result of APPLYING a learn against the DB. On kOk the cost was debited from
// `character.money` and the ability recorded in `learned`; `new_balance` is the
// balance AFTER the debit. On any rejection nothing changed and `new_balance` is the
// UNCHANGED balance.
struct LearnResult {
    TrainStatus   status = TrainStatus::kOk;
    std::uint32_t ability_id = 0;
    items::Copper cost = 0;
    items::Copper new_balance = 0;
};

// DB APPLY (NPC-02): learn `ability_id` from trainer `npc` for character `char_id`.
//
// Reads the character's live copper balance (currency::get_money), plans the learn
// via plan_learn using `char_class` / `char_level` and the in-memory `learned` set,
// and — on kOk ONLY — debits `cost` copper through currency::subtract_money (ONE
// row-locked characters-DB transaction) and records the ability in `learned`. On any
// rejection NOTHING is changed (all-or-nothing: money untouched, learned unchanged).
//
// Throws meridian::items::CharacterNotFound if `char_id` has no row, and
// meridian::db::DbError on a DB failure (the subtract transaction rolls back on any
// throw, so the balance is never left partially applied). A concurrent spend that
// makes the balance unaffordable between the plan and the debit surfaces as
// kInsufficientFunds (subtract_money's FOR UPDATE re-checks) rather than throwing.
LearnResult learn_ability(db::Connection& conn, std::uint64_t char_id,
                          const NpcDef& npc, std::uint32_t ability_id,
                          std::uint8_t char_class, std::uint16_t char_level,
                          LearnedAbilitySet& learned);

}  // namespace meridian::npc

#endif  // MERIDIAN_NPC_TRAINER_H
