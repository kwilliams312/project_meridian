// SPDX-License-Identifier: Apache-2.0
//
// worldd — movement-derived (fall + swim/breath) damage UNIT TEST (issue #362,
// CHR-02 full; part of epic #19).
//
// CLEAN-ROOM: written from docs/prd/server-prd.md §4-M1 ("CHR-02 (full): swim,
// fall damage"), movement_damage.h, map_tick.h, and combat_unit.h ONLY. No GPL /
// emulator source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE + DETERMINISTIC (no RNG): exercises the
// evaluator's arithmetic directly and its MapTick wiring, so it runs in the plain
// server ctest with no MariaDB (the #362 "seeded/deterministic tests" ask).
//
// What it proves:
//   A. FALL — a landing WITHIN the safe band does NO damage; a landing ABOVE it
//      takes damage scaled linearly by the excess height (as a fraction of max HP);
//      a hard enough fall is lethal; the apex is tracked across a multi-step arc.
//   B. WATER BREAKS A FALL — a fall that lands submerged deals no damage.
//   C. SWIM/BREATH — breath depletes underwater with NO damage until it is empty;
//      then drowning ticks on cadence for a fraction of max HP; SURFACING refills
//      the breath and stops the drowning.
//   D. MAPTICK WIRING — a landing / drowning evaluated in the combat phase applies
//      HP to the player's Unit via apply_damage (and stops once surfaced).

#include "combat_unit.h"
#include "map_tick.h"
#include "movement_constants.h"
#include "movement_damage.h"
#include "movement_validation.h"  // Position

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// A position at (x=y=0) and the given height z.
Position atz(float z) {
    Position p;
    p.x = 0.0f;
    p.y = 0.0f;
    p.z = z;
    return p;
}

// The M0 default environment: flat ground at kFlatGroundZ, no water.
MovementEnv flat_env() { return MovementEnv{}; }

// An environment with a water surface at `surface` (ground still flat at 0).
MovementEnv water_env(float surface) {
    MovementEnv e;
    e.water_surface_z = surface;
    return e;
}

UnitStats player_stats(std::uint32_t max_health) {
    UnitStats s;
    s.level = 5;
    s.max_health = max_health;
    s.faction = Faction::kPlayer;
    return s;
}

// Does the log contain a line whose text starts with `prefix`?
bool log_has(const MapTick& mt, const std::string& prefix) {
    for (const TickEvent& e : mt.log()) {
        if (e.text.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

int main() {
    std::printf("worldd movement-damage (fall + swim/breath) test — issue #362\n");

    constexpr std::uint32_t kMaxHp = 200;
    constexpr std::uint32_t kDt = mc::kTickMillis;  // 50 ms

    // -----------------------------------------------------------------------
    // A. FALL — safe band vs scaled vs lethal, and apex tracking.
    // Defaults: safe = 5 m, 0.10 max-HP per metre above safe. On 200 HP:
    //   fall 4 m  → 0 (within safe band)
    //   fall 10 m → 0.10 × (10-5) × 200 = 100
    //   fall 15 m → 0.10 × (15-5) × 200 = 200 (lethal)
    // -----------------------------------------------------------------------
    {
        MovementDamageState s;  // production params
        // Airborne at 4 m, then land: a 4 m drop is within the 5 m safe band.
        s.step(atz(4.0f), flat_env(), kDt, kMaxHp);           // seed (airborne)
        MovementDamageResult land = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        check("A: safe fall (4 m) registers a landing", land.landed);
        check("A: safe fall (4 m) deals NO damage", land.fall_damage == 0);
        check("A: safe-fall height is the drop", land.fall_height_m == 4.0f);
    }
    {
        MovementDamageState s;
        s.step(atz(10.0f), flat_env(), kDt, kMaxHp);          // seed at apex 10 m
        MovementDamageResult land = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        check("A: 10 m fall lands", land.landed);
        check("A: 10 m fall scales to 100 HP (0.10×(10-5)×200)", land.fall_damage == 100);
    }
    {
        MovementDamageState s;
        s.step(atz(15.0f), flat_env(), kDt, kMaxHp);
        MovementDamageResult land = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        check("A: 15 m fall is lethal-scale (== max HP)", land.fall_damage == kMaxHp);
    }
    {
        // Apex tracking across a rise-and-fall arc: apex 12 m, land at 0 ⇒ 12 m drop
        // ⇒ 0.10×(12-5)×200 = 140.
        MovementDamageState s;
        s.step(atz(2.0f), flat_env(), kDt, kMaxHp);   // seed airborne, apex 2
        s.step(atz(12.0f), flat_env(), kDt, kMaxHp);  // rise — apex → 12
        s.step(atz(6.0f), flat_env(), kDt, kMaxHp);   // descend — apex stays 12
        MovementDamageResult land = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        check("A: apex tracked across arc (12 m drop → 140 HP)", land.fall_damage == 140);
    }
    {
        // A grounded unit that never leaves the ground never takes fall damage
        // (the flat-map production case — no spurious damage).
        MovementDamageState s;
        MovementDamageResult a = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        MovementDamageResult b = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        check("A: staying grounded never lands / damages",
              !a.landed && !b.landed && a.fall_damage == 0 && b.fall_damage == 0);
    }

    // -----------------------------------------------------------------------
    // B. WATER BREAKS A FALL — a hard fall that lands submerged deals no damage.
    // Water surface at 100 m: any z below it is submerged (the whole fall + landing).
    // -----------------------------------------------------------------------
    {
        MovementDamageState s;
        s.step(atz(15.0f), water_env(100.0f), kDt, kMaxHp);   // seed (in the water column)
        MovementDamageResult land = s.step(atz(0.0f), water_env(100.0f), kDt, kMaxHp);
        check("B: a fall into water still registers a landing", land.landed);
        check("B: water breaks the fall (no damage despite 15 m drop)",
              land.fall_damage == 0 && land.fall_height_m == 15.0f);
    }

    // -----------------------------------------------------------------------
    // C. SWIM / BREATH — tightened params for a fast deterministic run:
    //   breath capacity 100 ms, drown interval 50 ms, 0.25 max-HP/tick, refill 2×.
    // On 200 HP a drown tick removes 0.25×200 = 50.
    // -----------------------------------------------------------------------
    {
        MovementDamageParams p;
        p.breath_capacity_ms = 100;
        p.drown_tick_interval_ms = 50;
        p.drown_tick_frac = 0.25f;
        p.breath_refill_rate = 2;
        MovementDamageState s(p);
        const MovementEnv env = water_env(10.0f);  // surface at 10 m; unit at z=0 ⇒ submerged

        // t1: breath 100 → 50, still air left ⇒ no drowning.
        MovementDamageResult r1 = s.step(atz(0.0f), env, kDt, kMaxHp);
        check("C: submerged this step", r1.submerged);
        check("C: breath depletes (100→50)", r1.breath_remaining_ms == 50);
        check("C: no drowning while breath remains", !r1.drowning && r1.drown_damage == 0);

        // t2: breath 50 → 0 exactly; the draining step is NOT charged as a drown tick.
        MovementDamageResult r2 = s.step(atz(0.0f), env, kDt, kMaxHp);
        check("C: breath empties (→0)", r2.breath_remaining_ms == 0);
        check("C: no drown tick on the step breath hits 0", !r2.drowning && r2.drown_damage == 0);

        // t3: breath already empty ⇒ a full 50 ms drown interval ⇒ one tick = 50 HP.
        MovementDamageResult r3 = s.step(atz(0.0f), env, kDt, kMaxHp);
        check("C: drowning begins once breath is exhausted", r3.drowning);
        check("C: one drown tick this step", r3.drown_ticks == 1);
        check("C: drown tick removes 0.25×maxHP = 50", r3.drown_damage == 50);

        // t4: surface (no water) ⇒ refill 2×50 = 100 ⇒ back to full; drowning stops.
        MovementDamageResult r4 = s.step(atz(0.0f), flat_env(), kDt, kMaxHp);
        check("C: surfacing clears the submerged flag", !r4.submerged);
        check("C: surfacing refills breath to capacity", r4.breath_remaining_ms == 100);
        check("C: no drowning while surfaced", !r4.drowning && r4.drown_damage == 0);
    }

    // -----------------------------------------------------------------------
    // D. MAPTICK WIRING — the combat phase applies the damage via Unit::apply_damage.
    // -----------------------------------------------------------------------
    {
        // Fall: spawn a player 10 m up, seed a tick, then land it. Expect 100 HP off
        // (200 → 100) and a fall_damage event.
        const AbilityStore abilities;  // empty store — no casts needed
        MapTick mt(abilities, /*rng_seed=*/0x5EED, /*dt_ms=*/kDt);
        const ObjectGuid g = mt.add_player(1, atz(10.0f), player_stats(kMaxHp));

        mt.advance();                       // tick 1: seed the tracker at 10 m (airborne)
        Unit* before = mt.unit_for_guid(g);
        check("D: player at full HP before landing",
              before != nullptr && before->health() == kMaxHp);

        mt.set_player_position(g, atz(0.0f));
        mt.advance();                       // tick 2: land ⇒ 10 m fall ⇒ 100 HP
        Unit* after = mt.unit_for_guid(g);
        check("D: landing applied 100 fall damage via apply_damage",
              after != nullptr && after->health() == kMaxHp - 100);
        check("D: a fall_damage event was emitted", log_has(mt, "fall_damage guid=1"));
    }
    {
        // Drowning: submerge a player and tick until drowning damage lands. Tighten
        // params (as in C) so it drowns within a few 50 ms ticks; on 200 HP each
        // drown tick removes 50. t1: 100→50 breath; t2: →0; t3: -50 (150); t4: -50 (100).
        const AbilityStore abilities;
        MapTick mt(abilities, /*rng_seed=*/0x5EED, /*dt_ms=*/kDt);

        MovementDamageParams p;
        p.breath_capacity_ms = 100;
        p.drown_tick_interval_ms = 50;
        p.drown_tick_frac = 0.25f;
        p.breath_refill_rate = 2;
        mt.set_movement_damage_params(p);   // must precede add_player
        mt.set_environment(water_env(10.0f));

        const ObjectGuid g = mt.add_player(2, atz(0.0f), player_stats(kMaxHp));
        mt.advance(4);                      // two drown ticks land (t3, t4)
        Unit* u = mt.unit_for_guid(g);
        check("D: drowning applied 2×50 HP via apply_damage",
              u != nullptr && u->health() == kMaxHp - 100);
        check("D: a drown_damage event was emitted", log_has(mt, "drown_damage guid=2"));
        check("D: still alive after two drown ticks", u != nullptr && u->is_alive());

        // Surface: no more water ⇒ drowning stops, HP holds steady.
        mt.set_environment(flat_env());
        mt.advance(4);
        u = mt.unit_for_guid(g);
        check("D: surfacing stops the drowning (HP holds)",
              u != nullptr && u->health() == kMaxHp - 100);

        // Back underwater long enough ⇒ eventually lethal (the death transition #342
        // owns; we only prove the damage path drives it, staying out of XP/respawn).
        mt.set_environment(water_env(10.0f));
        mt.advance(40);                     // ample ticks to exhaust the remaining 100 HP
        u = mt.unit_for_guid(g);
        check("D: sustained drowning is eventually lethal",
              u != nullptr && u->is_dead() && u->health() == 0);
        check("D: a DROWN death event was emitted", log_has(mt, "death guid=2 by=DROWN"));
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD MOVEMENT-DAMAGE TESTS PASSED\n"
                            : "\n%d WORLDD MOVEMENT-DAMAGE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
