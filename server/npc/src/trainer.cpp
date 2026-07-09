// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — the trainer path implementation (NPC-02; issue #372).
// See trainer.h for the pure-plan / DB-apply split. Clean-room, original
// (CONTRIBUTING.md).

#include "trainer.h"

#include "currency.h"  // meridian::items::get_money / subtract_money / InsufficientFunds

namespace meridian::npc {

const char* train_status_name(TrainStatus s) {
    switch (s) {
        case TrainStatus::kOk:                return "ok";
        case TrainStatus::kNotTrainer:        return "not_trainer";
        case TrainStatus::kUnknownAbility:    return "unknown_ability";
        case TrainStatus::kWrongClass:        return "wrong_class";
        case TrainStatus::kLevelTooLow:       return "level_too_low";
        case TrainStatus::kAlreadyKnown:      return "already_known";
        case TrainStatus::kInsufficientFunds: return "insufficient_funds";
    }
    return "unknown";
}

TrainPlan plan_learn(const NpcDef& npc, std::uint32_t ability_id,
                     std::uint8_t char_class, std::uint16_t char_level,
                     items::Copper balance, bool known) {
    TrainPlan plan;
    plan.ability_id = ability_id;

    // 1. The NPC must be a trainer at all.
    if (!npc.is_trainer) {
        plan.status = TrainStatus::kNotTrainer;
        return plan;
    }
    // 2. …and must actually teach this ability.
    const TrainerAbility* entry = npc.trainer_ability(ability_id);
    if (entry == nullptr) {
        plan.status = TrainStatus::kUnknownAbility;
        return plan;
    }
    plan.cost = entry->cost;  // echo the content cost from here on (server-computed)

    // 3. Not already known (nothing to sell twice).
    if (known) {
        plan.status = TrainStatus::kAlreadyKnown;
        return plan;
    }
    // 4. Right class (0 = any class may learn).
    if (entry->required_class != 0 && entry->required_class != char_class) {
        plan.status = TrainStatus::kWrongClass;
        return plan;
    }
    // 5. High enough level.
    if (char_level < entry->required_level) {
        plan.status = TrainStatus::kLevelTooLow;
        return plan;
    }
    // 6. Can afford it.
    if (balance < entry->cost) {
        plan.status = TrainStatus::kInsufficientFunds;
        return plan;
    }

    plan.status = TrainStatus::kOk;
    return plan;
}

LearnResult learn_ability(db::Connection& conn, std::uint64_t char_id,
                          const NpcDef& npc, std::uint32_t ability_id,
                          std::uint8_t char_class, std::uint16_t char_level,
                          LearnedAbilitySet& learned) {
    LearnResult result;
    result.ability_id = ability_id;

    // Read the live balance so the plan reflects the durable state, and so a
    // rejection can report the UNCHANGED balance back.
    const items::Copper balance = items::get_money(conn, char_id);
    result.new_balance = balance;

    const TrainPlan plan = plan_learn(npc, ability_id, char_class, char_level, balance,
                                      learned.knows(ability_id));
    result.status = plan.status;
    result.cost = plan.cost;
    if (plan.status != TrainStatus::kOk) {
        return result;  // all-or-nothing: nothing moved, balance unchanged
    }

    // kOk: debit the cost in ONE row-locked transaction, then record the learn.
    // subtract_money re-checks affordability under FOR UPDATE — a concurrent spend
    // that drained the balance since the plan surfaces as kInsufficientFunds here
    // (still nothing moved), rather than an underflow.
    try {
        result.new_balance = items::subtract_money(conn, char_id, plan.cost);
    } catch (const items::InsufficientFunds&) {
        result.status = TrainStatus::kInsufficientFunds;
        result.new_balance = balance;  // unchanged
        return result;
    }
    learned.learn(ability_id);
    return result;
}

}  // namespace meridian::npc
