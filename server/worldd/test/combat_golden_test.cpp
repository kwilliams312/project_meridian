// SPDX-License-Identifier: Apache-2.0
//
// worldd — combat tick + seeded GOLDEN sim-harness scenarios (issue #349; the
// epic-#18 capstone, CMB-01/02).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 (the per-map tick order),
// §3.2 (the one-map-tick sequence), §3.3 (the D-10 cast lifecycle) and the
// map_tick.h / combat_*.h / creature_ai.h / aura_container.h headers ONLY. No GPL /
// AGPL / CMaNGOS / TrinityCore / leaked emulator source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: drives the MapTick orchestrator (the single-
// threaded per-map tick) with a FIXED CombatRng seed, so every attack-table roll,
// damage/heal amount, and aura tick is deterministic and the whole event stream is
// BYTE-STABLE. Each scenario asserts its produced log against an embedded golden
// blob, so ANY drift in a combat formula, an attack-table band, an aura cadence,
// an AI transition, or the tick PHASE ORDER flips a golden line and fails CI.
// Always runs in the plain server ctest (no MERIDIAN_* env needed).
//
// The three scenarios prove the combat epic end-to-end:
//   A. ability use → GCD/accept → cast completes on a later tick → attack-table
//      roll → damage → DEATH. Proves the #354 cast-completion + resource-spend seam.
//   B. an instant-applied DoT TICKING over several ticks via the aura container.
//   C. a creature aggro → leash → EVADE → full-heal → resume-patrol → death →
//      RESPAWN cycle, driven by creature_ai in the AI phase.
//
// Set CAPTURE=1 in the environment to PRINT the produced stream for each scenario
// (used once to author the golden blobs below); with it unset the test asserts.

#include "map_tick.h"

#include "ability_store.h"
#include "combat_unit.h"
#include "creature_ai.h"
#include "movement_validation.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool capture() {
    const char* c = std::getenv("CAPTURE");
    return c != nullptr && c[0] == '1';
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

// A caster player: enough HP to never die, a mana pool for cast costs.
UnitStats caster_stats(std::uint16_t level, std::uint32_t mana) {
    UnitStats s;
    s.level = level;
    s.max_health = 500;
    s.resource_type = ResourceType::kMana;
    s.max_resource = mana;
    s.faction = Faction::kPlayer;
    return s;
}

// A hostile creature spawn def (the leash centre is `home`).
CreatureSpawnDef mob(Position home, std::uint16_t level, float aggro, float leash,
                     std::uint32_t respawn_ms, float move_speed) {
    CreatureSpawnDef d;
    d.template_id = 1;
    d.level = level;
    d.faction = Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = aggro;
    d.leash_radius = leash;
    d.respawn_ms = respawn_ms;
    d.move_speed = move_speed;
    d.patrol_mode = PatrolMode::kStationary;
    return d;
}

// Compare a produced stream to its golden, printing a unified-ish diff on mismatch
// (and always printing the raw stream in CAPTURE mode so goldens can be authored).
void assert_golden(const char* name, const std::string& actual, const std::string& golden) {
    if (capture()) {
        std::printf("----- CAPTURE %s -----\n%s\n----- END %s -----\n", name,
                    actual.c_str(), name);
        return;
    }
    const bool ok = (actual == golden);
    check(name, ok);
    if (!ok) {
        std::printf("--- expected (golden) ---\n%s\n--- actual ---\n%s\n", golden.c_str(),
                    actual.c_str());
    }
}

// ===========================================================================
// Scenario A — ability use → GCD/accept → cast completes → roll → damage → death.
// ===========================================================================
//
// A player nukes a low-HP creature. The 1.5 s cast is ACCEPTED (GCD armed, cast
// timer armed) on the drain-inbound phase of tick 1; it COMPLETES 30 ticks later
// (1500 ms at 20 Hz) in the combat/auras phase, which spends the mana, rolls the
// attack table, applies the damage, and drives the creature to 0 HP → death. The
// next AI phase projects the death as an EntityLeave{died} + the PATROL→DEAD edge.
std::string run_scenario_a() {
    const AbilityStore abilities = load_placeholder_ability_store();
    MapTick mt(abilities, /*rng_seed=*/0xA11CE5EEDULL, /*dt_ms=*/kTickDtMs);

    const ObjectGuid player = mt.add_player(1, at(0, 0), caster_stats(/*level=*/5, /*mana=*/200));
    const ObjectGuid crt = mt.add_creature(
        mob(at(10, 0), /*level=*/1, /*aggro=*/0, /*leash=*/1000, /*respawn_ms=*/999999,
            /*move_speed=*/0));
    // A one-nuke-kill dummy: cap HP below the nuke's minimum hit so any HIT/CRIT
    // is lethal (isolates the death transition from the damage roll's spread).
    mt.ai().creature(crt)->set_max_health(40);

    mt.enqueue_cast(AbilityUseCmd{player, kPlaceholderNukeId, crt});
    mt.advance(33);  // cast completes ~tick 31; +2 ticks for the AI death projection
    return mt.log_text();
}

// ===========================================================================
// Scenario B — a DoT ticking over several ticks via the aura container.
// ===========================================================================
//
// A player applies the instant Shadow DoT to a creature. The apply resolves in the
// drain-inbound phase of tick 1 (mana spent, aura added). The aura's 3 s periodic
// then fires in the combat/auras phase across the following ticks (dt = 1 s here,
// so the first tick lands at t = 3 s), dealing seeded damage each time, until the
// 12 s duration expires. No direct damage, no death — a pure periodic-aura proof.
std::string run_scenario_b() {
    const AbilityStore abilities = load_placeholder_ability_store();
    MapTick mt(abilities, /*rng_seed=*/0xD07D0700ULL, /*dt_ms=*/1000);

    const ObjectGuid player = mt.add_player(1, at(0, 0), caster_stats(/*level=*/5, /*mana=*/200));
    const ObjectGuid crt = mt.add_creature(
        mob(at(5, 0), /*level=*/5, /*aggro=*/0, /*leash=*/1000, /*respawn_ms=*/999999,
            /*move_speed=*/0));

    mt.enqueue_cast(AbilityUseCmd{player, kPlaceholderDotId, crt});
    mt.advance(13);  // 12 s duration at 1 s/tick → 4 periodic ticks + expiry
    return mt.log_text();
}

// ===========================================================================
// Scenario C — creature aggro → leash → evade → full-heal → respawn.
// ===========================================================================
//
// The creature-AI cycle, driven end-to-end through the tick (dt = 1 s):
//   1. player adjacent → melee hit wounds the creature + aggros it (PATROL→COMBAT);
//   2. player flees far → creature chases past its leash → EVADE (threat dropped),
//      runs home, FULL-HEALS on arrival → PATROL;
//   3. player returns → nukes the creature dead (PATROL→DEAD, EntityLeave);
//   4. the respawn timer elapses → the creature respawns at home, full HP,
//      patrolling (DEAD→PATROL, EntityEnter).
// Key states/HP are also asserted programmatically (below) — the golden pins the
// exact event stream + ordering.
struct ScenarioC {
    std::string log;
    bool aggroed = false;
    bool evaded = false;
    bool healed_full_on_evade = false;
    bool died = false;
    bool respawned_full = false;
};

ScenarioC run_scenario_c() {
    const AbilityStore abilities = load_placeholder_ability_store();
    MapTick mt(abilities, /*rng_seed=*/0xC0FFEE11ULL, /*dt_ms=*/1000);

    const ObjectGuid player = mt.add_player(1, at(5, 0), caster_stats(/*level=*/5, /*mana=*/500));
    const ObjectGuid crt = mt.add_creature(
        mob(at(0, 0), /*level=*/5, /*aggro=*/10, /*leash=*/20, /*respawn_ms=*/2000,
            /*move_speed=*/30));
    mt.ai().creature(crt)->set_max_health(40);  // one nuke hit is lethal (see A)
    const std::uint32_t max_hp = mt.ai().creature(crt)->max_health();

    ScenarioC out;

    // 1) aggro + wound: player adjacent (dist 5 ≤ aggro 10 and ≤ melee range 5).
    mt.enqueue_cast(AbilityUseCmd{player, kPlaceholderMeleeStrikeId, crt});
    mt.advance();  // inbound melee resolves (+threat), AI aggros
    out.aggroed = mt.ai().state_of(crt) == AiState::kCombat;
    const bool wounded = mt.ai().creature(crt)->health() < max_hp;

    // 2) flee: player far past the leash → creature chases then evades + full-heals.
    mt.set_player_position(player, at(100, 0));
    for (int i = 0; i < 6 && mt.ai().state_of(crt) != AiState::kPatrol; ++i) mt.advance();
    out.evaded = true;  // it must have passed through EVADE to be PATROL again (asserted via golden)
    out.healed_full_on_evade =
        (mt.ai().state_of(crt) == AiState::kPatrol) && wounded &&
        (mt.ai().creature(crt)->health() == max_hp);

    // 3) return + kill: player adjacent again, nuke until dead (deterministic).
    mt.set_player_position(player, at(2, 0));
    for (int c = 0; c < 4 && mt.ai().creature(crt)->is_alive(); ++c) {
        mt.enqueue_cast(AbilityUseCmd{player, kPlaceholderNukeId, crt});
        for (int i = 0; i < 3 && mt.ai().creature(crt)->is_alive(); ++i) mt.advance();
    }
    out.died = mt.ai().creature(crt)->is_dead();
    mt.advance();  // AI projects the death (EntityLeave + PATROL/COMBAT→DEAD)

    // 4) respawn: wait out the 2 s window (already partly elapsed) → respawn.
    for (int i = 0; i < 4 && mt.ai().state_of(crt) != AiState::kPatrol; ++i) mt.advance();
    out.respawned_full = mt.ai().creature(crt)->is_alive() &&
                         mt.ai().creature(crt)->health() == max_hp &&
                         mt.ai().state_of(crt) == AiState::kPatrol;

    out.log = mt.log_text();
    return out;
}

// ---------------------------------------------------------------------------
// Golden blobs (authored from a CAPTURE=1 run; see file header).
// ---------------------------------------------------------------------------
const char* const kGoldenA = R"GOLD(t=1 now=0 inbound cast_start caster=1 ability=4026531842 cast_ms=1500 ends=1500 target=13835058055282163712
t=31 now=1500 combat cast_complete caster=1 ability=4026531842 target=13835058055282163712
t=31 now=1500 combat resource_spend caster=1 amount=20 ok=1 left=180
t=31 now=1500 combat resolve caster=1 target=13835058055282163712 ability=4026531842 outcome=HIT amount=40 heal=0 target_hp=0 died=1
t=31 now=1500 combat death guid=13835058055282163712 by=1
t=32 now=1550 ai ai_leave guid=13835058055282163712 reason=died
t=32 now=1550 ai ai_state guid=13835058055282163712 PATROL->DEAD)GOLD";

const char* const kGoldenB = R"GOLD(t=1 now=0 inbound cast_accept caster=1 ability=4026531844 cast_ms=0 instant=1 target=13835058055282163712
t=1 now=0 inbound resource_spend caster=1 amount=15 ok=1 left=185
t=1 now=0 inbound aura_applied target=13835058055282163712 ability=4026531844 count=1
t=3 now=2000 aura aura_tick host=13835058055282163712 dmg=9 heal=0 ticks=1 expired=0 hp=161 host_died=0
t=6 now=5000 aura aura_tick host=13835058055282163712 dmg=9 heal=0 ticks=1 expired=0 hp=152 host_died=0
t=9 now=8000 aura aura_tick host=13835058055282163712 dmg=9 heal=0 ticks=1 expired=0 hp=143 host_died=0
t=12 now=11000 aura aura_tick host=13835058055282163712 dmg=6 heal=0 ticks=1 expired=1 hp=137 host_died=0)GOLD";

const char* const kGoldenC = R"GOLD(t=1 now=0 inbound cast_accept caster=1 ability=4026531841 cast_ms=0 instant=1 target=13835058055282163712
t=1 now=0 inbound resolve caster=1 target=13835058055282163712 ability=4026531841 outcome=HIT amount=11 heal=0 target_hp=29 died=0
t=1 now=0 ai ai_state guid=13835058055282163712 PATROL->COMBAT
t=3 now=2000 ai ai_state guid=13835058055282163712 COMBAT->EVADE
t=4 now=3000 ai ai_state guid=13835058055282163712 EVADE->PATROL
t=5 now=4000 inbound cast_start caster=1 ability=4026531842 cast_ms=1500 ends=5500 target=13835058055282163712
t=5 now=4000 ai ai_state guid=13835058055282163712 PATROL->COMBAT
t=7 now=6000 combat cast_complete caster=1 ability=4026531842 target=13835058055282163712
t=7 now=6000 combat resource_spend caster=1 amount=20 ok=1 left=480
t=7 now=6000 combat resolve caster=1 target=13835058055282163712 ability=4026531842 outcome=DODGE amount=0 heal=0 target_hp=40 died=0
t=8 now=7000 inbound cast_start caster=1 ability=4026531842 cast_ms=1500 ends=8500 target=13835058055282163712
t=10 now=9000 combat cast_complete caster=1 ability=4026531842 target=13835058055282163712
t=10 now=9000 combat resource_spend caster=1 amount=20 ok=1 left=460
t=10 now=9000 combat resolve caster=1 target=13835058055282163712 ability=4026531842 outcome=CRIT amount=40 heal=0 target_hp=0 died=1
t=10 now=9000 combat death guid=13835058055282163712 by=1
t=11 now=10000 ai ai_leave guid=13835058055282163712 reason=died
t=11 now=10000 ai ai_state guid=13835058055282163712 COMBAT->DEAD
t=13 now=12000 ai ai_enter guid=13835058055282163712 hp=40
t=13 now=12000 ai ai_state guid=13835058055282163712 DEAD->PATROL)GOLD";

}  // namespace

int main() {
    std::printf("worldd combat golden sim-harness (#349)\n");

    std::printf("A. ability use -> cast completes -> roll -> damage -> death\n");
    assert_golden("A: golden stream matches", run_scenario_a(), kGoldenA);

    std::printf("B. DoT ticking over several ticks\n");
    assert_golden("B: golden stream matches", run_scenario_b(), kGoldenB);

    std::printf("C. aggro -> leash -> evade -> full-heal -> respawn\n");
    ScenarioC c = run_scenario_c();
    check("C: creature aggroed on proximity/threat", c.aggroed);
    check("C: leashed -> evaded -> full-healed on arrival", c.healed_full_on_evade);
    check("C: nuked to death", c.died);
    check("C: respawned at home, full HP, patrolling", c.respawned_full);
    assert_golden("C: golden stream matches", c.log, kGoldenC);

    if (capture()) {
        std::printf("\nCAPTURE mode — goldens not asserted.\n");
        return 0;
    }
    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
