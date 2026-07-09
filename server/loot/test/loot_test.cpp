// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — DB-free unit test for the pure loot domain (ITM-02; issue
// #369). Written from the loot_table.h / loot_roll.h / loot_session.h headers and
// the meridian::items inventory API (#366) ONLY. No GPL/leaked source consulted
// (CONTRIBUTING.md).
//
// Proves the story's acceptance list deterministically:
//   * SEEDED DETERMINISM — a fixed seed yields a fixed roll (same twice; the
//     "deterministic roll for a seed" ask);
//   * STATISTICAL DROP RATES — a group's chance + weighted selection converge to
//     the configured rates over 10^5 draws (server PRD §7 "loot rolls —
//     statistical tests over 10^5 draws");
//   * PLACEHOLDER SET SHAPE — the M1 tables exist for the named creatures;
//   * LOOT SESSION VALIDATION — ownership, in-range, no-double-loot, and quest
//     gating, plus the transfer of a looted stack into a player inventory
//     (ownership moves corpse → player) and all-or-nothing on a full inventory.

#include <cstdio>
#include <string>

#include "inventory.h"
#include "item_template.h"
#include "loot_roll.h"
#include "loot_session.h"
#include "loot_table.h"

namespace li = meridian::items;
namespace lo = meridian::loot;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Expect a callable to throw a specific loot/inventory error type.
template <typename Ex, typename Fn>
void check_throws(const char* what, Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const Ex&) {
        threw = true;
    } catch (...) {
        threw = false;  // wrong type
    }
    check(what, threw);
}

// A single-group table used by the statistical tests: `chance` bp to drop one of
// two weighted entries (weights 3:1), quantity fixed at 1.
lo::LootTable weighted_table(std::uint32_t chance_bp) {
    lo::LootTable t;
    t.groups.push_back(lo::LootGroup{
        chance_bp,
        {
            lo::LootEntry{/*item*/ 100, 1, 1, /*weight*/ 3, 0},
            lo::LootEntry{/*item*/ 200, 1, 1, /*weight*/ 1, 0},
        }});
    return t;
}

// ---------------------------------------------------------------------------
void test_roll_determinism() {
    std::printf("roll: seeded determinism\n");
    lo::PlaceholderLootTableStore store;
    const lo::LootTable* t = store.find(lo::kCreatureWolf);
    check("wolf table exists", t != nullptr);

    lo::LootRng a(0xABCDEF12345ULL);
    lo::LootRng b(0xABCDEF12345ULL);
    lo::LootRoll ra = lo::roll_loot(*t, a);
    lo::LootRoll rb = lo::roll_loot(*t, b);

    bool same = ra.copper == rb.copper && ra.stacks.size() == rb.stacks.size();
    for (std::size_t i = 0; same && i < ra.stacks.size(); ++i)
        same = ra.stacks[i].item_template_id == rb.stacks[i].item_template_id &&
               ra.stacks[i].count == rb.stacks[i].count;
    check("same seed → identical roll", same);

    // The wolf's money range is 5..20 — a rolled amount is always in range.
    check("money within table range", ra.copper >= 5 && ra.copper <= 20);
}

void test_statistical_drop_rate() {
    std::printf("roll: statistical drop rate + weighted selection (10^5 draws)\n");
    constexpr int N = 100000;
    const lo::LootTable t = weighted_table(/*chance_bp=*/2500);  // 25%
    lo::LootRng rng(0x1234ULL);

    int hits = 0, item100 = 0, item200 = 0;
    for (int i = 0; i < N; ++i) {
        lo::LootRoll r = lo::roll_loot(t, rng);
        if (!r.stacks.empty()) {
            ++hits;
            if (r.stacks[0].item_template_id == 100) ++item100;
            else ++item200;
        }
    }
    const double rate = static_cast<double>(hits) / N;
    check("group drop rate ≈ 0.25 (±0.02)", rate > 0.23 && rate < 0.27);

    // Weighted selection 3:1 — item100 should be ≈ 3× item200.
    const double ratio = item200 > 0 ? static_cast<double>(item100) / item200 : 0.0;
    check("weighted 3:1 selection (ratio 2.6..3.4)", ratio > 2.6 && ratio < 3.4);
}

void test_never_and_always() {
    std::printf("roll: chance 0 never drops, chance 10000 always drops\n");
    lo::LootRng rng(7);
    const lo::LootTable never = weighted_table(0);
    const lo::LootTable always = weighted_table(lo::kLootRollScale);
    bool any_never = false, all_always = true;
    for (int i = 0; i < 1000; ++i) {
        if (!lo::roll_loot(never, rng).stacks.empty()) any_never = true;
        if (lo::roll_loot(always, rng).stacks.empty()) all_always = false;
    }
    check("chance 0 → never drops", !any_never);
    check("chance 10000 → always drops", all_always);
}

void test_placeholder_store() {
    std::printf("store: placeholder loot-table set shape\n");
    lo::PlaceholderLootTableStore store;
    check("wolf present", store.find(lo::kCreatureWolf) != nullptr);
    check("courier present", store.find(lo::kCreatureCourier) != nullptr);
    check("unknown creature → nullptr (no loot)", store.find(12345) == nullptr);
    auto ids = store.ids();
    check("ids ascending + count", ids.size() == 2 && ids[0] < ids[1]);

    // The courier's quest drop is tagged with the placeholder quest id.
    const lo::LootTable* c = store.find(lo::kCreatureCourier);
    bool has_quest_entry = false;
    for (const auto& g : c->groups)
        for (const auto& e : g.entries)
            if (e.required_quest_id == lo::kPlaceholderQuestId) has_quest_entry = true;
    check("courier has a quest-gated entry", has_quest_entry);
}

// Build a fixed roll: slot 0 a common potion stack (×3), slot 1 a quest item, 17
// copper. Used by the session tests so they are hermetic (no RNG).
lo::LootRoll fixed_roll() {
    lo::LootRoll r;
    const std::uint32_t potion = li::kPlaceholderIdBase + 7;   // Minor Health Potion
    const std::uint32_t dispatch = li::kPlaceholderIdBase + 9;  // Tattered Dispatch (quest)
    r.stacks.push_back(lo::LootStack{potion, 3, 0});
    r.stacks.push_back(lo::LootStack{dispatch, 1, lo::kPlaceholderQuestId});
    r.copper = 17;
    return r;
}

void test_session_ownership_and_range() {
    std::printf("session: ownership + in-range gates\n");
    li::PlaceholderTemplateStore templates;
    li::Inventory inv(templates);

    const lo::LooterId killer = 1001, stranger = 2002;
    const lo::LootPoint corpse{10.0f, 10.0f, 0.0f};
    lo::LootSession sess(/*corpse*/ 900, corpse, fixed_roll(), {killer});

    auto no_quest = [](std::uint32_t) { return false; };

    // A non-owner is rejected (ownership) even in range.
    check_throws<lo::NotAnOwner>("non-owner take → NotAnOwner", [&] {
        sess.take_item(stranger, corpse, 0, no_quest, inv);
    });
    check_throws<lo::NotAnOwner>("non-owner money → NotAnOwner",
                                 [&] { sess.take_money(stranger, corpse); });

    // The owner far from the corpse is rejected (in-range).
    const lo::LootPoint far{100.0f, 100.0f, 0.0f};
    check_throws<lo::LootOutOfRange>("owner out of range → LootOutOfRange", [&] {
        sess.take_item(killer, far, 0, no_quest, inv);
    });
    check("nothing was transferred on rejected pulls", inv.backpack_used() == 0);
}

void test_session_transfer_and_no_double_loot() {
    std::printf("session: transfer ownership + no double-loot\n");
    li::PlaceholderTemplateStore templates;
    li::Inventory inv(templates);

    const lo::LooterId killer = 1001;
    const lo::LootPoint corpse{0.0f, 0.0f, 0.0f};
    lo::LootSession sess(900, corpse, fixed_roll(), {killer});
    auto no_quest = [](std::uint32_t) { return false; };

    // Owner in range takes the common potion stack → it lands in the inventory.
    lo::LootStack got = sess.take_item(killer, corpse, 0, no_quest, inv);
    check("returned stack is the potion ×3", got.count == 3);
    check("inventory now holds the stack", inv.backpack_used() == 1);
    const li::ItemInstance* held = inv.backpack_at(0);
    check("held stack is potion ×3",
          held != nullptr && held->template_id == (li::kPlaceholderIdBase + 7) &&
              held->stack == 3);
    check("slot 0 now reads taken", sess.slot_taken_by(0, killer));

    // Taking it again is rejected (no double-loot) and adds nothing.
    check_throws<lo::LootAlreadyTaken>("second pull → LootAlreadyTaken", [&] {
        sess.take_item(killer, corpse, 0, no_quest, inv);
    });
    check("inventory unchanged after the rejected re-pull", inv.backpack_used() == 1);

    // Money: taken once, then rejected.
    li::Copper money = sess.take_money(killer, corpse);
    check("money was 17", money == 17);
    check("money reads taken", sess.copper_taken());
    check_throws<lo::LootAlreadyTaken>("second money grab → LootAlreadyTaken",
                                       [&] { sess.take_money(killer, corpse); });
}

void test_session_quest_gating() {
    std::printf("session: quest-gated drop only for eligible players\n");
    li::PlaceholderTemplateStore templates;

    // Two eligible looters of the same corpse (share the kill / threat group).
    const lo::LooterId qplayer = 11, noqplayer = 22;
    const lo::LootPoint corpse{0.0f, 0.0f, 0.0f};
    lo::LootSession sess(900, corpse, fixed_roll(), {qplayer, noqplayer});

    auto on_quest = [](std::uint32_t q) { return q == lo::kPlaceholderQuestId; };
    auto off_quest = [](std::uint32_t) { return false; };

    // The player NOT on the quest cannot even see the quest slot (slot 1).
    auto vis_off = sess.visible_slots(noqplayer, off_quest);
    bool sees_quest_off = false;
    for (const auto& v : vis_off) if (v.is_quest()) sees_quest_off = true;
    check("player off-quest does NOT see the quest slot", !sees_quest_off);

    // …and a pull of it is rejected with QUEST_REQUIRED.
    li::Inventory inv_off(templates);
    check_throws<lo::LootQuestRequired>("off-quest pull → LootQuestRequired", [&] {
        sess.take_item(noqplayer, corpse, 1, off_quest, inv_off);
    });

    // The player ON the quest sees it and can take their personal copy.
    auto vis_on = sess.visible_slots(qplayer, on_quest);
    bool sees_quest_on = false;
    for (const auto& v : vis_on) if (v.is_quest()) sees_quest_on = true;
    check("player on-quest sees the quest slot", sees_quest_on);

    li::Inventory inv_on(templates);
    sess.take_item(qplayer, corpse, 1, on_quest, inv_on);
    check("quest item transferred to the eligible player", inv_on.backpack_used() == 1);

    // Quest loot is PERSONAL: the other eligible player (if also on-quest) still
    // gets their own copy — one looter taking it does not consume it for another.
    li::Inventory inv_on2(templates);
    sess.take_item(noqplayer, corpse, 1, on_quest, inv_on2);  // now on-quest too
    check("quest item is personal (second eligible looter also gets one)",
          inv_on2.backpack_used() == 1);

    // A common slot, by contrast, is shared — once taken it is gone for everyone.
    li::Inventory shared(templates);
    sess.take_item(qplayer, corpse, 0, on_quest, shared);
    check_throws<lo::LootAlreadyTaken>("common slot is shared (2nd looter blocked)", [&] {
        sess.take_item(noqplayer, corpse, 0, on_quest, shared);
    });
}

void test_session_inventory_full_all_or_nothing() {
    std::printf("session: full inventory → InventoryFull, loot stays on corpse\n");
    li::PlaceholderTemplateStore templates;
    li::Inventory inv(templates, /*backpack_capacity=*/1);

    // Fill the single backpack slot with a non-stackable weapon.
    li::ItemInstance filler;
    filler.template_id = li::kPlaceholderIdBase + 1;  // Worn Shortsword (max_stack 1)
    filler.stack = 1;
    inv.add(filler);
    check("backpack is full (1/1)", inv.backpack_free() == 0);

    // A loot roll of a DIFFERENT non-stackable item cannot fit.
    lo::LootRoll r;
    r.stacks.push_back(lo::LootStack{li::kPlaceholderIdBase + 2, 1, 0});  // Cracked Buckler
    const lo::LooterId killer = 1;
    const lo::LootPoint corpse{0, 0, 0};
    lo::LootSession sess(900, corpse, r, {killer});
    auto no_quest = [](std::uint32_t) { return false; };

    check_throws<li::InventoryFull>("full inventory → InventoryFull", [&] {
        sess.take_item(killer, corpse, 0, no_quest, inv);
    });
    // All-or-nothing: the slot is NOT consumed — the loot is still on the corpse.
    check("loot stays on corpse after the failed pull", !sess.slot_taken_by(0, killer));
    check("session not fully looted", !sess.fully_looted());
}

void test_fully_looted() {
    std::printf("session: fully-looted transitions (corpse may despawn)\n");
    li::PlaceholderTemplateStore templates;
    li::Inventory inv(templates);
    const lo::LooterId killer = 1;
    const lo::LootPoint corpse{0, 0, 0};
    lo::LootSession sess(900, corpse, fixed_roll(), {killer});
    auto no_quest = [](std::uint32_t) { return false; };
    auto on_quest = [](std::uint32_t q) { return q == lo::kPlaceholderQuestId; };

    check("not looted at start", !sess.fully_looted());
    sess.take_item(killer, corpse, 0, no_quest, inv);  // common potion
    sess.take_money(killer, corpse);                    // money
    // Quest slot (personal) does NOT keep the corpse alive.
    check("fully looted once shared loot + money gone", sess.fully_looted());
    // The eligible looter can still grab their personal quest item afterwards.
    li::Inventory inv2(templates);
    sess.take_item(killer, corpse, 1, on_quest, inv2);
    check("quest item still takeable after fully_looted", inv2.backpack_used() == 1);
}

}  // namespace

int main() {
    std::printf("meridian-loot unit test (ITM-02; #369)\n");
    test_roll_determinism();
    test_statistical_drop_rate();
    test_never_and_always();
    test_placeholder_store();
    test_session_ownership_and_range();
    test_session_transfer_and_no_double_loot();
    test_session_quest_gating();
    test_session_inventory_full_all_or_nothing();
    test_fully_looted();

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
