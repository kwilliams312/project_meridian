// SPDX-License-Identifier: Apache-2.0
//
// worldd — quest state machine tests (issue #371, QST-01; the spine of the M1
// game loop, epic #20).
//
// CLEAN-ROOM: written from the quest_def.h / quest_log.h / map_tick.h / inventory.h
// headers and docs/{prd,sad}/server-*.md + quest.schema.yaml ONLY. No GPL / AGPL /
// CMaNGOS / TrinityCore / leaked emulator quest logic consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE / DETERMINISTIC: the whole quest core runs against
// the placeholder quest chain + placeholder item templates with no MariaDB, no
// socket, no clock. Two groups of scenarios:
//   A. the pure state machine (quest_log.h): accept gating (level + prerequisite),
//      each objective source (kill / collect / deliver / explore), can't-turn-in-
//      incomplete, wrong-NPC turn-in, and the reward grant (items minted into the
//      inventory + XP/copper amounts).
//   B. the kill objective END-TO-END through the MapTick on_unit_died hook — the
//      "kill count via on_unit_died" seam (#359/#365) the issue calls for.

#include "quest_log.h"

#include "ability_store.h"
#include "combat_unit.h"
#include "creature_ai.h"
#include "inventory.h"
#include "item_template.h"
#include "leveling.h"
#include "map_tick.h"
#include "movement_validation.h"
#include "quest_def.h"

#include <cstdio>
#include <string>

using namespace meridian::worldd;
namespace it = meridian::items;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Placeholder content ids (derived from the public seam constants — the same ids
// the PlaceholderQuestStore / PlaceholderTemplateStore author).
constexpr QuestId       kQ1 = kPlaceholderQuestIdBase + 1;  // kill: cull 3 kobolds
constexpr QuestId       kQ2 = kPlaceholderQuestIdBase + 2;  // collect: 5 copper ore
constexpr QuestId       kQ3 = kPlaceholderQuestIdBase + 3;  // deliver: dispatch -> courier
constexpr QuestId       kQ4 = kPlaceholderQuestIdBase + 4;  // explore: ridge (gated)
constexpr std::uint32_t kSergeant = kPlaceholderNpcIdBase + 1;
constexpr std::uint32_t kSmith    = kPlaceholderNpcIdBase + 2;
constexpr std::uint32_t kCourier  = kPlaceholderNpcIdBase + 3;
constexpr std::uint32_t kKobold   = kPlaceholderNpcIdBase + 10;
constexpr std::uint32_t kZone     = kPlaceholderZoneIdBase + 1;
constexpr std::uint32_t kPotion   = it::kPlaceholderIdBase + 7;  // Minor Health Potion (stack 20)
constexpr std::uint32_t kOre      = it::kPlaceholderIdBase + 8;  // Copper Ore (stack 100)
constexpr std::uint32_t kDispatch = it::kPlaceholderIdBase + 9;  // Tattered Dispatch
constexpr std::uint32_t kSword    = it::kPlaceholderIdBase + 1;  // Worn Shortsword (choice)

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

// ===========================================================================
// A. Accept gating — level + prerequisite + duplicate/unknown.
// ===========================================================================
void scenario_accept_gating() {
    std::printf("A1. accept gating: level + prerequisite + duplicate/unknown\n");
    PlaceholderQuestStore quests;
    QuestLog log(quests);

    check("unknown quest is rejected",
          log.can_accept(/*id=*/999999, /*level=*/10) == AcceptStatus::kUnknownQuest);

    // Q4 gates on level 2 AND completing Q1 — at level 1 it is level-gated.
    check("Q4 rejected below required_level",
          log.can_accept(kQ4, /*level=*/1) == AcceptStatus::kLevelTooLow);
    // At level 2 the level gate clears but the prerequisite (Q1) is unmet.
    check("Q4 rejected without its prerequisite",
          log.can_accept(kQ4, /*level=*/2) == AcceptStatus::kMissingPrerequisite);

    check("Q1 accepts (entry quest, no gate)",
          log.accept(kQ1, /*level=*/1) == AcceptStatus::kOk);
    check("Q1 is now active", log.is_active(kQ1));
    check("re-accepting an active quest is rejected",
          log.accept(kQ1, /*level=*/1) == AcceptStatus::kAlreadyActive);
}

// ===========================================================================
// A. Kill objective — accept → on_kill×3 → turn-in → reward (XP + copper + items).
// ===========================================================================
void scenario_kill() {
    std::printf("A2. kill objective: accept → progress → turn-in → reward\n");
    PlaceholderQuestStore quests;
    it::PlaceholderTemplateStore templates;
    QuestLog log(quests);
    it::Inventory inv(templates);

    check("accept Q1", log.accept(kQ1, 1) == AcceptStatus::kOk);

    // Turning in before the objective is met is refused (can't-turn-in-incomplete).
    RewardGrant early;
    check("turn-in refused while incomplete",
          log.turn_in(kQ1, kSergeant, inv, -1, early) == TurnInStatus::kIncomplete);

    // A kill of a NON-target creature does not advance the objective.
    check("unrelated kill does not advance", !log.on_kill(kKobold + 5));

    // Three kobold kills complete the 3-kill objective; a 4th does not overshoot.
    check("kill 1 advances", log.on_kill(kKobold));
    check("kill 2 advances", log.on_kill(kKobold));
    check("not complete after 2 kills", !log.is_complete(kQ1));
    check("kill 3 advances", log.on_kill(kKobold));
    check("complete after 3 kills", log.is_complete(kQ1));
    check("kill 4 does not advance a capped objective", !log.on_kill(kKobold));

    // Turn-in at the WRONG NPC is refused (the sergeant is the giver AND turn-in;
    // the courier is not).
    RewardGrant wrong;
    check("turn-in at the wrong NPC is refused",
          log.turn_in(kQ1, kCourier, inv, -1, wrong) == TurnInStatus::kWrongNpc);

    // Turn-in at the giver grants the reward: 2 potions minted + 50 XP + 120 copper.
    RewardGrant r;
    check("turn-in succeeds", log.turn_in(kQ1, kSergeant, inv, -1, r) == TurnInStatus::kOk);
    check("reward XP is 50", r.xp == 50);
    check("reward money is 120 copper", r.money == it::Copper{120});
    check("2 potions minted into the inventory", count_items(inv, kPotion) == 2);
    check("quest is now completed", log.is_completed(kQ1));
    check("quest no longer active", !log.is_active(kQ1));
    check("re-accepting a completed quest is rejected",
          log.accept(kQ1, 1) == AcceptStatus::kAlreadyCompleted);
}

// ===========================================================================
// A. Collect objective — inventory-driven progress + a reward CHOICE.
// ===========================================================================
void scenario_collect() {
    std::printf("A3. collect objective: inventory sync → turn-in with a choice\n");
    PlaceholderQuestStore quests;
    it::PlaceholderTemplateStore templates;
    QuestLog log(quests);
    it::Inventory inv(templates);

    check("accept Q2", log.accept(kQ2, 1) == AcceptStatus::kOk);

    // No ore yet → sync leaves it incomplete; turn-in refused.
    RewardGrant early;
    check("turn-in refused with no ore",
          log.turn_in(kQ2, kSmith, inv, /*choice=*/0, early) == TurnInStatus::kIncomplete);

    // Partial ore: 3 of 5 → still incomplete.
    it::ItemInstance ore;
    ore.template_id = kOre;
    ore.stack = 3;
    inv.add(ore);
    check("3 ore does not complete a 5-collect", (log.sync_collect(inv), !log.is_complete(kQ2)));

    // Top up to 5 → complete.
    it::ItemInstance more;
    more.template_id = kOre;
    more.stack = 2;
    inv.add(more);
    check("5 ore completes the collect", (log.sync_collect(inv), log.is_complete(kQ2)));

    // A choice-reward quest with a bad/absent choice index is refused.
    RewardGrant bad;
    check("missing reward choice is refused",
          log.turn_in(kQ2, kSmith, inv, /*choice=*/-1, bad) == TurnInStatus::kBadChoice);
    check("out-of-range reward choice is refused",
          log.turn_in(kQ2, kSmith, inv, /*choice=*/9, bad) == TurnInStatus::kBadChoice);

    // Valid choice 0 → Worn Shortsword minted.
    RewardGrant r;
    check("turn-in with choice 0 succeeds",
          log.turn_in(kQ2, kSmith, inv, /*choice=*/0, r) == TurnInStatus::kOk);
    check("reward XP is 40", r.xp == 40);
    check("chosen reward item (shortsword) minted", count_items(inv, kSword) == 1);
    check("exactly one reward item granted (the choice)", r.items.size() == 1);
    // Turn-in CONSUMES the collect objective's items (handed over) — the 5 ore are
    // removed from the inventory and reported for the durable layer to persist.
    check("collect items consumed on turn-in (no ore left)", count_items(inv, kOre) == 0);
    check("turn-in reports the consumed collect items",
          r.consumed.size() == 1 && r.consumed[0].item_id == kOre && r.consumed[0].count == 5);
}

// ===========================================================================
// A3b. Consumption frees room: a backpack with no free slot still turns in a
// collect quest because handing over the collected items frees their slot.
// ===========================================================================
void scenario_collect_consumption_frees_room() {
    std::printf("A3b. turn-in consumes collect items → frees the room for the reward\n");
    PlaceholderQuestStore quests;
    it::PlaceholderTemplateStore templates;
    QuestLog log(quests);
    // A tiny 1-slot backpack: the only slot holds the 5 collected ore. Without
    // consumption the single reward (the choice item) would not fit (kInventoryFull);
    // consuming the ore frees the slot so the turn-in succeeds.
    it::Inventory inv(templates, /*backpack_capacity=*/1);

    check("accept Q2", log.accept(kQ2, 1) == AcceptStatus::kOk);
    it::ItemInstance ore;
    ore.template_id = kOre;
    ore.stack = 5;
    inv.add(ore);
    check("backpack is full (1/1 slot)", inv.backpack_free() == 0);
    check("collect completes", (log.sync_collect(inv), log.is_complete(kQ2)));

    RewardGrant r;
    check("turn-in succeeds despite a full backpack (consumption frees the slot)",
          log.turn_in(kQ2, kSmith, inv, /*choice=*/0, r) == TurnInStatus::kOk);
    check("the ore is gone, the reward took its slot", count_items(inv, kOre) == 0);
    check("the reward item occupies the freed slot", count_items(inv, kSword) == 1);
}

// ===========================================================================
// A. Deliver objective — on_deliver + turn-in at the courier (not the giver).
// ===========================================================================
void scenario_deliver() {
    std::printf("A4. deliver objective: on_deliver → turn-in at the turn-in NPC\n");
    PlaceholderQuestStore quests;
    it::PlaceholderTemplateStore templates;
    QuestLog log(quests);
    it::Inventory inv(templates);

    check("accept Q3", log.accept(kQ3, 1) == AcceptStatus::kOk);

    // Delivering the wrong item / to the wrong NPC does not complete it.
    check("wrong deliver target does not advance", !log.on_deliver(kSergeant, kDispatch));
    check("still incomplete", !log.is_complete(kQ3));

    check("delivering the dispatch to the courier advances",
          log.on_deliver(kCourier, kDispatch));
    check("deliver objective complete", log.is_complete(kQ3));

    // Q3 turns in at the COURIER (turn_in defaults away from the giver here).
    RewardGrant atgiver;
    check("turn-in at the giver is refused (turn-in NPC is the courier)",
          log.turn_in(kQ3, kSergeant, inv, -1, atgiver) == TurnInStatus::kWrongNpc);

    RewardGrant r;
    check("turn-in at the courier succeeds",
          log.turn_in(kQ3, kCourier, inv, -1, r) == TurnInStatus::kOk);
    check("reward XP is 30", r.xp == 30);
}

// ===========================================================================
// A. Explore objective — the full gated chain: Q1 unlocks Q4 at level 2.
// ===========================================================================
void scenario_explore_chain() {
    std::printf("A5. explore objective + the prerequisite chain (Q1 → Q4)\n");
    PlaceholderQuestStore quests;
    it::PlaceholderTemplateStore templates;
    QuestLog log(quests);
    it::Inventory inv(templates);

    // Complete Q1 first (unlocks Q4's prerequisite).
    log.accept(kQ1, 5);
    log.on_kill(kKobold);
    log.on_kill(kKobold);
    log.on_kill(kKobold);
    RewardGrant r1;
    log.turn_in(kQ1, kSergeant, inv, -1, r1);
    check("Q1 completed (prerequisite satisfied)", log.is_completed(kQ1));

    // Now Q4 accepts at level 2.
    check("Q4 still level-gated at level 1",
          log.can_accept(kQ4, 1) == AcceptStatus::kLevelTooLow);
    check("Q4 accepts at level 2 with its prerequisite met",
          log.accept(kQ4, 2) == AcceptStatus::kOk);

    // Exploring the wrong POI does nothing; the right one completes it.
    check("wrong POI does not advance", !log.on_explore(kZone, "wrong_poi"));
    check("discovering the ridge advances", log.on_explore(kZone, "kobold_ridge"));
    check("explore objective complete", log.is_complete(kQ4));

    RewardGrant r;
    check("turn-in Q4 succeeds", log.turn_in(kQ4, kSergeant, inv, -1, r) == TurnInStatus::kOk);
    check("reward XP is 60", r.xp == 60);
    check("reward money is 40 copper", r.money == it::Copper{40});
}

// ===========================================================================
// B. Kill objective END-TO-END through the MapTick on_unit_died hook.
// ===========================================================================
UnitStats attacker_stats(std::uint16_t level) {
    UnitStats s;
    s.level = level;
    s.max_health = 1000;
    s.resource_type = ResourceType::kMana;
    s.max_resource = 1000;
    s.faction = Faction::kPlayer;
    return s;
}

CreatureSpawnDef kobold(Position home) {
    CreatureSpawnDef d;
    d.template_id = kKobold;  // matches the Q1 kill objective target
    d.level = 1;
    d.faction = Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = 0;
    d.leash_radius = 1000;
    d.respawn_ms = 999999;
    d.move_speed = 0;
    d.patrol_mode = PatrolMode::kStationary;
    return d;
}

// Count the kCreatureKill events in a MapTick log crediting `killer` for a `npc`
// template death (the typed event-bus payload the world loop routes; #396).
std::size_t count_kill_events(const MapTick& mt, ObjectGuid killer, std::uint32_t npc) {
    std::size_t n = 0;
    for (const TickEvent& ev : mt.log()) {
        if (ev.kind == TickEventKind::kCreatureKill && ev.killer_guid == killer &&
            ev.npc_template_id == npc)
            ++n;
    }
    return n;
}

// Grind three kobolds through a MapTick and return how many died (durable player).
int grind_three_kobolds(MapTick& mt, ObjectGuid pl) {
    int killed = 0;
    for (int i = 0; i < 3; ++i) {
        const ObjectGuid crt = mt.add_creature(kobold(at(2, 0)));
        Unit* cu = mt.unit_for_guid(crt);
        cu->set_max_health(8);  // one melee strike is lethal
        for (int t = 0; t < 12 && cu->is_alive(); ++t) {
            mt.enqueue_cast(AbilityUseCmd{pl, kPlaceholderMeleeStrikeId, crt});
            mt.advance();
        }
        if (cu->is_dead()) ++killed;
    }
    return killed;
}

void scenario_map_tick_kill_hook() {
    std::printf("B. MapTick reports creature kills as typed events (QST-01 event-bus; #396)\n");
    const AbilityStore abilities = load_placeholder_ability_store();

    // --- reporting ENABLED: each creature death emits a typed kCreatureKill --------
    // carrying the killer guid + victim template id (the world loop routes this to
    // the killer's SESSION; MapTick no longer owns any quest log).
    {
        MapTick mt(abilities, /*rng_seed=*/0x0371ULL, /*dt_ms=*/1600);
        mt.set_report_kills(true);
        const ObjectGuid pl = mt.add_player(1, at(0, 0), attacker_stats(5));
        check("three kobolds died", grind_three_kobolds(mt, pl) == 3);
        check("exactly three kCreatureKill events crediting the killer",
              count_kill_events(mt, pl, kKobold) == 3);
    }

    // --- reporting DISABLED (the default): no kill event is emitted, so combat/death
    // golden scenarios are byte-identical. ---------------------------------------
    {
        MapTick mt(abilities, /*rng_seed=*/0x0371ULL, /*dt_ms=*/1600);
        const ObjectGuid pl = mt.add_player(1, at(0, 0), attacker_stats(5));
        check("three kobolds died", grind_three_kobolds(mt, pl) == 3);
        std::size_t kill_events = 0;
        for (const TickEvent& ev : mt.log())
            if (ev.kind == TickEventKind::kCreatureKill) ++kill_events;
        check("no kCreatureKill events when reporting is off", kill_events == 0);
        check("no quest_kill text line when reporting is off",
              mt.log_text().find("quest_kill") == std::string::npos);
    }
}

}  // namespace

int main() {
    std::printf("worldd quest state machine (QST-01 #371)\n");
    scenario_accept_gating();
    scenario_kill();
    scenario_collect();
    scenario_collect_consumption_frees_room();
    scenario_deliver();
    scenario_explore_chain();
    scenario_map_tick_kill_hook();
    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
