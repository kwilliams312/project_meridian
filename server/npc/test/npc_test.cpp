// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc — DB-free unit test for the pure NPC-interaction domain (NPC-01/02;
// issue #372). Written from the npc_def.h / gossip.h / trainer.h headers ONLY. No
// GPL/leaked source consulted (CONTRIBUTING.md).
//
// Proves the story's acceptance list deterministically (no DB, no socket):
//   * GOSSIP OPTIONS REFLECT STATE — a quest-available NPC shows accept; once the
//     quest is in the log it shows in-progress; once complete it shows turn-in;
//     the vendor/trainer role flags surface their options; option order is
//     deterministic and combined options coexist.
//   * TRAINER PLAN — teaches on valid class + level + copper; rejects on wrong
//     class, too-low level, insufficient funds, already-known, non-trainer, and
//     unknown ability. `balance` is a pure input here, so a rejection never even
//     computes a debit (the DB debit is proven in trainer_db_it.cpp).
//   * PLACEHOLDER SET SHAPE — the M1 NPCs exist and carry the expected roles.

#include <cstdio>

#include "gossip.h"
#include "npc_def.h"
#include "trainer.h"

namespace mn = meridian::npc;
namespace mi = meridian::items;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// A fake QuestStateView: each of the three states is a settable flag for one quest.
// Lets a test dial the player's quest state without a real QuestLog (the seam the
// gossip planner depends on — see gossip.h).
class FakeQuestState : public mn::QuestStateView {
public:
    std::uint32_t quest = 0;
    bool acceptable = false;
    bool active = false;
    bool complete = false;

    bool can_accept(std::uint32_t q) const override { return q == quest && acceptable; }
    bool is_active(std::uint32_t q) const override { return q == quest && active; }
    bool is_complete(std::uint32_t q) const override { return q == quest && complete; }
};

// Whether `menu` contains exactly one option of `kind` for `target` (quest id; 0
// for role options).
bool has_option(const mn::GossipMenu& menu, mn::GossipOptionKind kind,
                std::uint32_t target) {
    int n = 0;
    for (const auto& o : menu.options)
        if (o.kind == kind && o.target_id == target) ++n;
    return n == 1;
}

void test_placeholder_shape() {
    std::printf("[placeholder NPC set]\n");
    mn::PlaceholderNpcStore store;
    check("has three placeholder NPCs", store.ids().size() == 3);
    check("ids ascending", store.ids().front() == mn::kNpcQuestGiver &&
                               store.ids().back() == mn::kNpcVendor);

    const mn::NpcDef* giver = store.find(mn::kNpcQuestGiver);
    check("quest giver found", giver != nullptr);
    check("quest giver gives+takes the placeholder quest",
          giver && giver->quests.size() == 1 &&
              giver->quests[0].quest_id == mn::kPlaceholderQuestId &&
              giver->quests[0].gives && giver->quests[0].turn_in);
    check("quest giver is neither vendor nor trainer",
          giver && !giver->is_vendor && !giver->is_trainer);

    const mn::NpcDef* trainer = store.find(mn::kNpcTrainer);
    check("trainer found + flagged", trainer && trainer->is_trainer);
    check("trainer teaches two abilities",
          trainer && trainer->trainer_abilities.size() == 2);
    check("trainer teaches the class-gated strike",
          trainer && trainer->trainer_ability(mn::kTrainedStrike) &&
              trainer->trainer_ability(mn::kTrainedStrike)->required_class ==
                  mn::kClassVanguard);
    check("trainer teaches the any-class heal",
          trainer && trainer->trainer_ability(mn::kTrainedHeal) &&
              trainer->trainer_ability(mn::kTrainedHeal)->required_class == 0);

    const mn::NpcDef* vendor = store.find(mn::kNpcVendor);
    check("vendor found + flagged", vendor && vendor->is_vendor && !vendor->is_trainer);
    check("unknown npc id -> nullptr", store.find(123456u) == nullptr);
}

void test_gossip_quest_states() {
    std::printf("[gossip — quest options reflect state]\n");
    mn::PlaceholderNpcStore store;
    const mn::NpcDef& giver = *store.find(mn::kNpcQuestGiver);
    FakeQuestState qs;
    qs.quest = mn::kPlaceholderQuestId;

    // Available: can accept, not active/complete -> the accept option.
    qs.acceptable = true;
    {
        mn::GossipMenu m = mn::build_gossip_menu(giver, qs);
        check("available -> exactly one option", m.options.size() == 1);
        check("available -> kQuestAvailable for the quest",
              has_option(m, mn::GossipOptionKind::kQuestAvailable, mn::kPlaceholderQuestId));
    }

    // In progress: active, not complete -> the in-progress option (no accept).
    qs.acceptable = false;
    qs.active = true;
    {
        mn::GossipMenu m = mn::build_gossip_menu(giver, qs);
        check("in-progress -> kQuestInProgress",
              has_option(m, mn::GossipOptionKind::kQuestInProgress, mn::kPlaceholderQuestId));
        check("in-progress -> no accept option",
              !has_option(m, mn::GossipOptionKind::kQuestAvailable, mn::kPlaceholderQuestId));
    }

    // Complete + this NPC is the turn-in -> the turn-in option.
    qs.complete = true;
    {
        mn::GossipMenu m = mn::build_gossip_menu(giver, qs);
        check("complete -> kQuestComplete (turn in here)",
              has_option(m, mn::GossipOptionKind::kQuestComplete, mn::kPlaceholderQuestId));
        check("complete -> not shown as in-progress",
              !has_option(m, mn::GossipOptionKind::kQuestInProgress, mn::kPlaceholderQuestId));
    }

    // Already turned in / not acceptable / not active / not complete -> no option.
    qs.acceptable = false;
    qs.active = false;
    qs.complete = false;
    {
        mn::GossipMenu m = mn::build_gossip_menu(giver, qs);
        check("done/gated -> no quest option at all", m.options.empty());
    }
}

void test_gossip_role_flags_and_order() {
    std::printf("[gossip — vendor/trainer flags + order]\n");
    mn::PlaceholderNpcStore store;
    FakeQuestState none;  // quest 0 — matches nothing

    // Vendor: just the vendor option.
    {
        mn::GossipMenu m = mn::build_gossip_menu(*store.find(mn::kNpcVendor), none);
        check("vendor NPC -> exactly the vendor option",
              m.options.size() == 1 &&
                  has_option(m, mn::GossipOptionKind::kVendor, 0));
    }

    // Trainer NPC that also gives a quest, with the quest available: quest option
    // FIRST, then the trainer option (deterministic order).
    {
        FakeQuestState qs;
        qs.quest = mn::kPlaceholderQuestId;
        qs.acceptable = true;
        mn::GossipMenu m = mn::build_gossip_menu(*store.find(mn::kNpcTrainer), qs);
        check("trainer+quest -> two options", m.options.size() == 2);
        check("quest option comes before the trainer option",
              m.options.size() == 2 &&
                  m.options[0].kind == mn::GossipOptionKind::kQuestAvailable &&
                  m.options[1].kind == mn::GossipOptionKind::kTrainer);
    }
}

// A trainer NPC + its two abilities, for the plan tests.
void test_trainer_plan() {
    std::printf("[trainer — learn plan]\n");
    mn::PlaceholderNpcStore store;
    const mn::NpcDef& trainer = *store.find(mn::kNpcTrainer);
    const mn::NpcDef& giver = *store.find(mn::kNpcQuestGiver);

    constexpr std::uint8_t kVanguard = mn::kClassVanguard;  // 1
    constexpr std::uint8_t kRuncaller = 2;

    // Valid: Vanguard, level >= 2, can afford 50 -> ok, cost echoed.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, mn::kTrainedStrike, kVanguard,
                                         /*level=*/2, /*balance=*/50, /*known=*/false);
        check("valid strike -> kOk", p.status == mn::TrainStatus::kOk);
        check("valid strike -> cost 50", p.cost == 50);
    }

    // Wrong class: a Runcaller cannot learn the Vanguard strike.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, mn::kTrainedStrike, kRuncaller,
                                         /*level=*/9, /*balance=*/9999, /*known=*/false);
        check("wrong class -> kWrongClass", p.status == mn::TrainStatus::kWrongClass);
    }

    // Too-low level: Vanguard but below required level 2.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, mn::kTrainedStrike, kVanguard,
                                         /*level=*/1, /*balance=*/9999, /*known=*/false);
        check("level too low -> kLevelTooLow", p.status == mn::TrainStatus::kLevelTooLow);
    }

    // Insufficient funds: right class + level, but 49 < 50.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, mn::kTrainedStrike, kVanguard,
                                         /*level=*/5, /*balance=*/49, /*known=*/false);
        check("insufficient funds -> kInsufficientFunds",
              p.status == mn::TrainStatus::kInsufficientFunds);
    }

    // Already known: even a fully-eligible learner is rejected.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, mn::kTrainedStrike, kVanguard,
                                         /*level=*/5, /*balance=*/9999, /*known=*/true);
        check("already known -> kAlreadyKnown", p.status == mn::TrainStatus::kAlreadyKnown);
    }

    // Any-class ability: a Runcaller may learn the heal at level 5 with 120 copper.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, mn::kTrainedHeal, kRuncaller,
                                         /*level=*/5, /*balance=*/120, /*known=*/false);
        check("any-class heal -> kOk for a Runcaller", p.status == mn::TrainStatus::kOk);
        check("any-class heal -> cost 120", p.cost == 120);
    }

    // Not a trainer: the quest-giver NPC teaches nothing.
    {
        mn::TrainPlan p = mn::plan_learn(giver, mn::kTrainedStrike, kVanguard,
                                         /*level=*/9, /*balance=*/9999, /*known=*/false);
        check("non-trainer NPC -> kNotTrainer", p.status == mn::TrainStatus::kNotTrainer);
    }

    // Unknown ability: the trainer does not teach this id.
    {
        mn::TrainPlan p = mn::plan_learn(trainer, /*ability=*/0xDEADBEEFu, kVanguard,
                                         /*level=*/9, /*balance=*/9999, /*known=*/false);
        check("unknown ability -> kUnknownAbility",
              p.status == mn::TrainStatus::kUnknownAbility);
    }
}

// The in-memory learned set (M1 — no durable table yet).
void test_quest_marker_lifecycle() {
    std::printf("[quest marker — overhead icon across a quest lifecycle]\n");
    mn::PlaceholderNpcStore store;
    // The placeholder giver both GIVES and TURNS IN the one placeholder quest — the
    // common giver==turn-in NPC, so one NPC walks the whole marker lifecycle.
    const mn::NpcDef& giver = *store.find(mn::kNpcQuestGiver);
    const mn::NpcDef& vendor = *store.find(mn::kNpcVendor);  // no quests at all
    FakeQuestState qs;
    qs.quest = mn::kPlaceholderQuestId;

    // Before accept: acceptable -> `!` available.
    qs.acceptable = true;
    check("available before accept -> kAvailable",
          mn::compute_quest_marker(giver, qs) == mn::QuestMarker::kAvailable);

    // After accept (in log, objectives incomplete): greyed `?` at the turn-in NPC.
    qs.acceptable = false;
    qs.active = true;
    check("accepted+incomplete -> kTurnInIncomplete (greyed ?)",
          mn::compute_quest_marker(giver, qs) == mn::QuestMarker::kTurnInIncomplete);

    // Objectives complete: lit `?` ready to turn in.
    qs.complete = true;
    check("complete -> kTurnInReady (lit ?)",
          mn::compute_quest_marker(giver, qs) == mn::QuestMarker::kTurnInReady);

    // After turn-in (no longer acceptable/active/complete): no marker.
    qs.acceptable = false;
    qs.active = false;
    qs.complete = false;
    check("after turn-in -> kNone",
          mn::compute_quest_marker(giver, qs) == mn::QuestMarker::kNone);

    // An NPC that gives/takes no quest never shows a marker, whatever the state.
    qs.acceptable = true;
    qs.active = true;
    qs.complete = true;
    check("non-quest NPC -> kNone",
          mn::compute_quest_marker(vendor, qs) == mn::QuestMarker::kNone);

    // Precedence: a ready turn-in outranks an available quest (both true at once is
    // impossible for one quest, so verify the fold picks the lit `?` when complete).
    qs.acceptable = true;
    qs.active = true;
    qs.complete = true;
    check("complete dominates -> kTurnInReady",
          mn::compute_quest_marker(giver, qs) == mn::QuestMarker::kTurnInReady);
}

void test_learned_set() {
    std::printf("[trainer — in-memory learned set]\n");
    mn::LearnedAbilitySet learned;
    check("empty initially", !learned.knows(mn::kTrainedStrike) && learned.size() == 0);
    check("learn returns true (newly added)", learned.learn(mn::kTrainedStrike));
    check("knows after learn", learned.knows(mn::kTrainedStrike));
    check("re-learn returns false (already present)", !learned.learn(mn::kTrainedStrike));
    check("size is 1", learned.size() == 1);
}

}  // namespace

int main() {
    test_placeholder_shape();
    test_gossip_quest_states();
    test_gossip_role_flags_and_order();
    test_trainer_plan();
    test_quest_marker_lifecycle();
    test_learned_set();

    std::printf(g_fail == 0 ? "\nALL NPC TESTS PASSED\n"
                            : "\n%d NPC TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
