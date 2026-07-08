// SPDX-License-Identifier: Apache-2.0
//
// worldd — death + XP END-TO-END through the single MapTick "unit died" hook
// (issues #359 CMB-03 + #360 CHR-03; part of epic #19).
//
// CLEAN-ROOM: written from the map_tick.h / death_state.h / leveling.h headers and
// docs/{prd,sad}/server-*.md ONLY. No GPL / AGPL / CMaNGOS / TrinityCore / leaked
// emulator source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: drives the MapTick orchestrator with a fixed
// CombatRng seed. Two seeded/deterministic scenarios prove the two stories that
// share the death hook:
//   1. DEATH CYCLE (#359): a player dies → corpse spawns → graveyard release
//      (requested + auto) → corpse-run → resurrect (health restored, corpse
//      despawned), plus the TOO_FAR gate.
//   2. XP / LEVELING (#360): a player kills creatures and levels 1→5 fully
//      server-authoritatively, with stat growth applied from the level curve.

#include "map_tick.h"

#include "ability_store.h"
#include "combat_unit.h"
#include "creature_ai.h"
#include "death_state.h"
#include "leveling.h"
#include "movement_validation.h"

#include <cstdio>
#include <string>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

// A durable attacker: enough HP to never die, plenty of resource.
UnitStats attacker_stats(std::uint16_t level) {
    UnitStats s;
    s.level = level;
    s.max_health = 1000;
    s.resource_type = ResourceType::kMana;
    s.max_resource = 1000;
    s.faction = Faction::kPlayer;
    return s;
}

// A hostile, low-HP Player used to exercise the PLAYER death path through the
// resolver (a player may harm a kHostile target — combat_resolver can_attack).
UnitStats hostile_victim_stats(std::uint32_t hp) {
    UnitStats s;
    s.level = 1;
    s.max_health = hp;
    s.resource_type = ResourceType::kNone;
    s.faction = Faction::kHostile;
    return s;
}

CreatureSpawnDef mob(Position home, std::uint16_t level) {
    CreatureSpawnDef d;
    d.template_id = 1;
    d.level = level;
    d.faction = Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = 0;      // no proximity aggro (threat from the hit pulls it)
    d.leash_radius = 1000;        // never leashes in this test
    d.respawn_ms = 999999;        // effectively never respawns
    d.move_speed = 0;             // stationary
    d.patrol_mode = PatrolMode::kStationary;
    return d;
}

// Melee `victim` from `attacker` until it dies (or the budget runs out). dt is set
// so the GCD clears each tick (dt > kGlobalCooldownMs), one strike per tick.
void melee_until_dead(MapTick& mt, ObjectGuid attacker, ObjectGuid victim, Unit* victim_unit,
                      int budget) {
    for (int i = 0; i < budget && victim_unit->is_alive(); ++i) {
        mt.enqueue_cast(AbilityUseCmd{attacker, kPlaceholderMeleeStrikeId, victim});
        mt.advance();
    }
}

// ===========================================================================
// Scenario 1 — player death → corpse → release → corpse-run → resurrect (#359).
// ===========================================================================
void scenario_death_cycle() {
    std::printf("1. death → corpse → release → corpse-run → resurrect (#359)\n");

    const AbilityStore abilities = load_placeholder_ability_store();
    MapTick mt(abilities, /*rng_seed=*/0xDEAD0359ULL, /*dt_ms=*/1600);
    mt.set_graveyard(at(50, 50));

    const ObjectGuid atk = mt.add_player(1, at(0, 0), attacker_stats(5));
    const ObjectGuid vic = mt.add_player(2, at(3, 0), hostile_victim_stats(8));
    Unit* vu = mt.unit_for_guid(vic);

    // --- die ---
    melee_until_dead(mt, atk, vic, vu, /*budget=*/12);
    check("victim died", vu->is_dead());
    check("victim entered the death FSM as a corpse",
          mt.deaths().phase_of(vic) == DeathPhase::kCorpse);
    const DeathRecord* rec = mt.deaths().record(vic);
    check("a corpse was spawned", rec != nullptr && rec->corpse_guid != 0);
    const ObjectGuid corpse_guid = rec != nullptr ? rec->corpse_guid : 0;
    check("corpse is at the death spot (3,0)",
          rec != nullptr && rec->corpse_pos.x == 3 && rec->corpse_pos.y == 0);
    check("corpse object exists in the machine", mt.deaths().corpse(corpse_guid) != nullptr);

    // --- requested graveyard release → ghost, teleported to the graveyard ---
    mt.request_release(vic);
    mt.advance();
    check("released → ghost", mt.deaths().phase_of(vic) == DeathPhase::kGhost);
    check("ghost teleported to the graveyard (50,50)",
          vu->position().x == 50 && vu->position().y == 50);

    // --- resurrect refused far from the corpse (corpse-run not complete) ---
    mt.request_resurrect(vic);
    mt.advance();
    check("resurrect refused away from the corpse (still ghost, still dead)",
          mt.deaths().phase_of(vic) == DeathPhase::kGhost && vu->is_dead());

    // --- corpse-run: walk the ghost back to the corpse, then resurrect ---
    mt.set_player_position(vic, at(3, 0));  // arrived at the corpse
    mt.request_resurrect(vic);
    mt.advance();
    check("resurrected at the corpse", vu->is_alive());
    check("no longer in the death FSM", mt.deaths().phase_of(vic) == DeathPhase::kAlive);
    check("health restored to 50% of max (4/8)",
          vu->health() == 4 && vu->max_health() == 8);
    check("corpse despawned on resurrect", mt.deaths().corpse(corpse_guid) == nullptr);
    check("no dead players remain", mt.deaths().dead_count() == 0);
}

// ===========================================================================
// Scenario 1b — auto-release when the player never requests it (#359).
// ===========================================================================
void scenario_auto_release() {
    std::printf("1b. auto graveyard release on the timer (#359)\n");

    const AbilityStore abilities = load_placeholder_ability_store();
    MapTick mt(abilities, /*rng_seed=*/0xA070ULL, /*dt_ms=*/1600);
    mt.set_graveyard(at(-20, 0));

    const ObjectGuid atk = mt.add_player(1, at(0, 0), attacker_stats(5));
    const ObjectGuid vic = mt.add_player(2, at(3, 0), hostile_victim_stats(8));
    Unit* vu = mt.unit_for_guid(vic);
    melee_until_dead(mt, atk, vic, vu, /*budget=*/12);
    check("victim is a corpse awaiting release",
          mt.deaths().phase_of(vic) == DeathPhase::kCorpse);

    // Default auto_release_ms is 6000; at dt=1600 it elapses within ~4 ticks.
    for (int i = 0; i < 6 && mt.deaths().phase_of(vic) == DeathPhase::kCorpse; ++i)
        mt.advance();
    check("auto-released to ghost once the timer elapsed",
          mt.deaths().phase_of(vic) == DeathPhase::kGhost);
    check("auto-released ghost is at the graveyard (-20,0)",
          vu->position().x == -20 && vu->position().y == 0);
}

// ===========================================================================
// Scenario 2 — kill → XP → level-up 1→5 with stat growth (#360).
// ===========================================================================
void scenario_leveling() {
    std::printf("2. kill → XP → level-up 1→5 with stat growth (#360)\n");

    const AbilityStore abilities = load_placeholder_ability_store();
    MapTick mt(abilities, /*rng_seed=*/0x1E7E1ULL, /*dt_ms=*/1600);

    // A level-1 Vanguard (roster class 1) — the class curve drives stat growth.
    const std::uint8_t kVanguard = 1;
    const ObjectGuid pl =
        mt.add_player(1, at(0, 0), placeholder_player_stats(kVanguard, 1), kVanguard);
    Unit* pu = mt.unit_for_guid(pl);

    const std::uint16_t start_level = pu->level();
    const std::uint32_t start_hp = pu->max_health();
    check("starts at level 1", start_level == 1);

    // Grind level-1 creatures until level 5 (server-authoritative XP + level-ups).
    int kills = 0;
    for (int guard = 0; guard < 500 && pu->level() < 5; ++guard) {
        const ObjectGuid crt = mt.add_creature(mob(at(2, 0), /*level=*/1));
        Unit* cu = mt.unit_for_guid(crt);
        cu->set_max_health(8);  // one melee strike is lethal
        const std::uint16_t before = pu->level();
        melee_until_dead(mt, pl, crt, cu, /*budget=*/12);
        if (cu->is_dead()) ++kills;
        (void)before;
    }
    std::printf("  ..    (leveled 1→%u in %d kills)\n", pu->level(), kills);

    check("player reached level 5 by killing", pu->level() == 5);

    // Stat growth applied from the level curve: L5 Vanguard health > L1, and it
    // matches the placeholder class/level curve exactly (the world-DB-curve seam).
    const std::uint32_t l5_hp = placeholder_player_stats(kVanguard, 5).max_health;
    check("max health grew with level", pu->max_health() > start_hp);
    check("max health matches the L5 class curve", pu->max_health() == l5_hp);
    check("healed to full on level-up", pu->health() == pu->max_health());

    // The event stream recorded the awards + exactly four level-ups (1→2→3→4→5).
    const std::string log = mt.log_text();
    auto count = [&](const std::string& needle) {
        std::size_t n = 0, pos = 0;
        while ((pos = log.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };
    check("XP awards were emitted", count("xp_award killer=1") >= 1);
    check("exactly four level-ups (1→5)", count("level_up guid=1") == 4);
}

}  // namespace

int main() {
    std::printf("worldd death + XP end-to-end (single death hook; #359 + #360)\n");
    scenario_death_cycle();
    scenario_auto_release();
    scenario_leveling();
    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
