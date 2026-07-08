// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-Unit combat aura container UNIT TEST (issue #346, CMB-01; epic #18).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 (the CMB-01 aura-container
// line "periodics, stat mods, stacking" + §3.2 "combat + auras (casts complete,
// periodics, deaths)"), the IF-4 world DDL (schema/sql/world/30_ability.sql
// duration_ms / max_stacks / periodic_* + ability_effect_stat_mod), and
// aura_container.h / combat_unit.h / ability_store.h. No GPL / emulator source
// consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE: drives AuraContainer directly with a
// seeded CombatRng and an explicit dt, so it runs in the PLAIN server ctest (no
// MariaDB) and is fully deterministic. Wiring into the 20 Hz tick loop is #349.
//
// What it proves:
//   A. APPLY / REFRESH / EXPIRE — an aura applies, refreshes (duration reset), and
//      expires exactly when its duration elapses.
//   B. PERIODIC cadence + application — a DoT/HoT fires on its tick_ms cadence and
//      moves the host's HP via Unit::apply_damage/apply_healing; a coarse dt of
//      N·tick is equivalent to N steps of dt = tick (accumulator catch-up).
//   C. STAT-MOD apply / remove — a stat-mod aura contributes a net delta while
//      active and rolls it back on expiry / remove / clear.
//   D. STACKING — refresh-duration (max_stacks==1), stack-count (max_stacks>1,
//      capped, scaling periodic + stat delta), independent instances (per caster).
//   E. DEATH + DETERMINISM — a periodic kill reports host_died and stops further
//      ticks; a ranged periodic is identical across equal (seed, dt) runs.

#include "aura_container.h"

#include "ability_store.h"
#include "combat_resolver.h"  // CombatRng
#include "combat_unit.h"
#include "movement_validation.h"  // Position

#include <cstdint>
#include <cstdio>

using namespace meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// A live Unit with `max_health` HP (full, alive) at the origin.
Unit make_unit(std::uint32_t max_health) {
    UnitStats s;
    s.level = 1;
    s.max_health = max_health;
    s.faction = Faction::kHostile;
    Unit u(/*guid=*/1, ObjectType::kCreature, Position{}, s);
    return u;
}

// Build a one-effect aura Ability. Defaults give a pure (non-periodic, no-stat)
// timed aura; callers set periodic_* / stat_mods / max_stacks as needed.
Ability make_aura_ability(AbilityId id, std::uint32_t duration_ms,
                          std::uint16_t max_stacks = 1) {
    Ability a;
    a.id = id;
    a.name = "test-aura";
    AbilityEffect e;
    e.kind = EffectKind::kAura;
    e.duration_ms = duration_ms;
    e.max_stacks = max_stacks;
    a.effects.push_back(e);
    return a;
}

}  // namespace

int main() {
    std::printf("worldd aura container unit test (issue #346)\n");

    // ===== A. APPLY / REFRESH / EXPIRE ======================================
    {
        Unit u = make_unit(100);
        AuraContainer c(u);
        CombatRng rng(1);

        Ability buff = make_aura_ability(1001, /*duration=*/5000);
        AuraApplyResult r = c.apply(buff, 0, /*caster=*/7);
        check("A: fresh apply returns kAdded", r.action == AuraApplyAction::kAdded);
        check("A: apply sets stacks 1", r.stacks == 1);
        check("A: container size 1", c.size() == 1);
        check("A: find locates the instance", c.find(1001, 0, 7) != nullptr);
        check("A: wrong caster does not match", c.find(1001, 0, 99) == nullptr);

        // Tick partway — still present, remaining wound down.
        AuraTickResult t1 = c.tick(2000, rng);
        check("A: partial tick expires nothing", t1.auras_expired == 0);
        const ActiveAura* a = c.find(1001, 0, 7);
        check("A: remaining wound to 3000", a && a->remaining_ms == 3000);

        // Re-apply (max_stacks==1) refreshes duration back to full.
        AuraApplyResult r2 = c.apply(buff, 0, 7);
        check("A: re-apply returns kRefreshed", r2.action == AuraApplyAction::kRefreshed);
        check("A: still one instance", c.size() == 1);
        a = c.find(1001, 0, 7);
        check("A: refresh reset remaining to 5000", a && a->remaining_ms == 5000);

        // Tick exactly to expiry.
        c.tick(4000, rng);
        check("A: not yet expired at 1000 left", c.size() == 1);
        AuraTickResult t3 = c.tick(1000, rng);
        check("A: expires exactly at duration end", t3.auras_expired == 1);
        check("A: container empty after expiry", c.empty());
    }

    // ===== B. PERIODIC cadence + application =================================
    {
        // DoT: 10 dmg every 2000 ms for 6000 ms → 3 ticks, 30 total.
        Unit u = make_unit(100);
        AuraContainer c(u);
        CombatRng rng(1);

        Ability dot = make_aura_ability(2001, /*duration=*/6000);
        dot.effects[0].periodic_kind = PeriodicKind::kDamage;
        dot.effects[0].periodic_amount_min = 10;
        dot.effects[0].periodic_amount_max = 10;  // fixed → cadence test is RNG-free
        dot.effects[0].periodic_tick_ms = 2000;
        c.apply(dot, 0, /*caster=*/7);

        AuraTickResult s1 = c.tick(2000, rng);
        check("B: first 2000ms step fires one tick", s1.ticks_fired == 1);
        check("B: tick applied 10 damage", s1.periodic_damage == 10);
        check("B: host at 90 HP", u.health() == 90);

        AuraTickResult s2 = c.tick(2000, rng);
        check("B: second step fires one tick", s2.ticks_fired == 1);
        check("B: host at 80 HP", u.health() == 80);

        AuraTickResult s3 = c.tick(2000, rng);
        check("B: final boundary tick fires", s3.ticks_fired == 1);
        check("B: host at 70 HP", u.health() == 70);
        check("B: aura expired at duration end", s3.auras_expired == 1 && c.empty());

        // Catch-up equivalence: one coarse dt = 6000 fires the same 3 ticks.
        Unit u2 = make_unit(100);
        AuraContainer c2(u2);
        CombatRng rng2(1);
        c2.apply(dot, 0, 7);
        AuraTickResult big = c2.tick(6000, rng2);
        check("B: coarse dt=6000 fires 3 ticks", big.ticks_fired == 3);
        check("B: coarse dt applied 30 damage", big.periodic_damage == 30);
        check("B: host at 70 HP after coarse tick", u2.health() == 70);
        check("B: aura expired after coarse tick", big.auras_expired == 1);

        // HoT: heal a wounded host.
        Unit u3 = make_unit(100);
        u3.apply_damage(50);  // 50 HP
        AuraContainer c3(u3);
        CombatRng rng3(1);
        Ability hot = make_aura_ability(2002, /*duration=*/4000);
        hot.effects[0].periodic_kind = PeriodicKind::kHeal;
        hot.effects[0].periodic_amount_min = 8;
        hot.effects[0].periodic_amount_max = 8;
        hot.effects[0].periodic_tick_ms = 2000;
        c3.apply(hot, 0, 7);
        AuraTickResult h = c3.tick(4000, rng3);
        check("B: HoT fires 2 ticks", h.ticks_fired == 2);
        check("B: HoT healed 16", h.periodic_healing == 16);
        check("B: host healed to 66 HP", u3.health() == 66);
    }

    // ===== C. STAT-MOD apply / remove =======================================
    {
        Unit u = make_unit(100);
        AuraContainer c(u);
        CombatRng rng(1);

        Ability buff = make_aura_ability(3001, /*duration=*/3000);
        buff.effects[0].stat_mods.push_back(StatMod{StatKey::kStrength, +15});
        buff.effects[0].stat_mods.push_back(StatMod{StatKey::kStamina, -5});
        c.apply(buff, 0, /*caster=*/7);

        check("C: strength delta +15 while active", c.stat_delta(StatKey::kStrength) == 15);
        check("C: stamina delta -5 while active", c.stat_delta(StatKey::kStamina) == -5);
        check("C: untouched stat is 0", c.stat_delta(StatKey::kAgility) == 0);

        // Explicit remove rolls the deltas back.
        bool removed = c.remove(3001, 0, 7);
        check("C: remove found the aura", removed);
        check("C: strength delta back to 0 after remove", c.stat_delta(StatKey::kStrength) == 0);
        check("C: stamina delta back to 0 after remove", c.stat_delta(StatKey::kStamina) == 0);

        // Re-apply, then let it EXPIRE — expiry must also roll the deltas back.
        c.apply(buff, 0, 7);
        check("C: strength +15 again after re-apply", c.stat_delta(StatKey::kStrength) == 15);
        c.tick(3000, rng);
        check("C: strength delta 0 after expiry", c.stat_delta(StatKey::kStrength) == 0);
        check("C: container empty after expiry", c.empty());
    }

    // ===== D. STACKING ======================================================
    {
        // D1 — refresh-duration (max_stacks == 1): re-apply never stacks.
        {
            Unit u = make_unit(100);
            AuraContainer c(u);
            CombatRng rng(1);
            Ability a = make_aura_ability(4001, 5000, /*max_stacks=*/1);
            c.apply(a, 0, 7);
            AuraApplyResult r = c.apply(a, 0, 7);
            check("D1: re-apply is kRefreshed", r.action == AuraApplyAction::kRefreshed);
            check("D1: stays 1 stack", r.stacks == 1);
            check("D1: single instance", c.size() == 1);
        }

        // D2 — stack-count (max_stacks == 3): stacks grow, cap, scale.
        {
            Unit u = make_unit(1000);
            AuraContainer c(u);
            CombatRng rng(1);
            Ability a = make_aura_ability(4002, 5000, /*max_stacks=*/3);
            a.effects[0].stat_mods.push_back(StatMod{StatKey::kAgility, +4});
            a.effects[0].periodic_kind = PeriodicKind::kDamage;
            a.effects[0].periodic_amount_min = 5;
            a.effects[0].periodic_amount_max = 5;
            a.effects[0].periodic_tick_ms = 1000;

            AuraApplyResult r1 = c.apply(a, 0, 7);
            check("D2: first apply kAdded 1 stack", r1.action == AuraApplyAction::kAdded && r1.stacks == 1);
            AuraApplyResult r2 = c.apply(a, 0, 7);
            check("D2: second apply kStacked 2 stacks", r2.action == AuraApplyAction::kStacked && r2.stacks == 2);
            AuraApplyResult r3 = c.apply(a, 0, 7);
            check("D2: third apply kStacked 3 stacks", r3.action == AuraApplyAction::kStacked && r3.stacks == 3);
            AuraApplyResult r4 = c.apply(a, 0, 7);
            check("D2: fourth apply capped, kRefreshed 3 stacks",
                  r4.action == AuraApplyAction::kRefreshed && r4.stacks == 3);
            check("D2: still one instance", c.size() == 1);

            // Stat delta scales with stacks: 3 × +4 = +12.
            check("D2: agility delta scales to +12", c.stat_delta(StatKey::kAgility) == 12);

            // Periodic scales with stacks: one tick = 5 × 3 = 15.
            AuraTickResult t = c.tick(1000, rng);
            check("D2: one periodic tick fired", t.ticks_fired == 1);
            check("D2: periodic scaled by stacks (15)", t.periodic_damage == 15);
        }

        // D3 — independent instances: same ability effect, two casters.
        {
            Unit u = make_unit(1000);
            AuraContainer c(u);
            CombatRng rng(1);
            Ability a = make_aura_ability(4003, 4000, /*max_stacks=*/1);
            a.effects[0].periodic_kind = PeriodicKind::kDamage;
            a.effects[0].periodic_amount_min = 7;
            a.effects[0].periodic_amount_max = 7;
            a.effects[0].periodic_tick_ms = 2000;

            AuraApplyResult ra = c.apply(a, 0, /*casterA=*/10);
            AuraApplyResult rb = c.apply(a, 0, /*casterB=*/20);
            check("D3: caster A kAdded", ra.action == AuraApplyAction::kAdded);
            check("D3: caster B kAdded (independent)", rb.action == AuraApplyAction::kAdded);
            check("D3: two independent instances", c.size() == 2);
            check("D3: both instances findable", c.find(4003, 0, 10) && c.find(4003, 0, 20));

            // Both tick independently → 7 + 7 = 14 in one 2000 ms step.
            AuraTickResult t = c.tick(2000, rng);
            check("D3: both instances tick (2 ticks)", t.ticks_fired == 2);
            check("D3: combined periodic damage 14", t.periodic_damage == 14);
        }
    }

    // ===== E. DEATH + DETERMINISM ===========================================
    {
        // E1 — a periodic kill reports host_died and stops further ticks.
        Unit u = make_unit(12);
        AuraContainer c(u);
        CombatRng rng(1);
        Ability dot = make_aura_ability(5001, /*duration=*/10000);
        dot.effects[0].periodic_kind = PeriodicKind::kDamage;
        dot.effects[0].periodic_amount_min = 5;
        dot.effects[0].periodic_amount_max = 5;
        dot.effects[0].periodic_tick_ms = 1000;
        c.apply(dot, 0, 7);

        // 12 HP, 5/tick: ticks at 1s(→7), 2s(→2), 3s(→dead). A coarse dt=10000 must
        // stop after the lethal tick — not keep rolling on a corpse.
        AuraTickResult t = c.tick(10000, rng);
        check("E1: host reported dead", t.host_died);
        check("E1: host at 0 HP", u.health() == 0 && u.is_dead());
        check("E1: only 3 ticks landed (stopped on death)", t.ticks_fired == 3);
        check("E1: total damage capped at host HP (12)", t.periodic_damage == 12);

        // E2 — a ranged periodic is identical across equal (seed, dt) runs and
        // stays within [min,max] per tick.
        Ability rdot = make_aura_ability(5002, /*duration=*/8000);
        rdot.effects[0].periodic_kind = PeriodicKind::kDamage;
        rdot.effects[0].periodic_amount_min = 1;
        rdot.effects[0].periodic_amount_max = 100;
        rdot.effects[0].periodic_tick_ms = 2000;

        auto run = [&](std::uint64_t seed) {
            Unit uu = make_unit(100000);
            AuraContainer cc(uu);
            CombatRng r(seed);
            cc.apply(rdot, 0, 7);
            std::uint32_t total = 0;
            for (int i = 0; i < 4; ++i) total += cc.tick(2000, r).periodic_damage;
            return total;
        };
        const std::uint32_t a = run(12345);
        const std::uint32_t b = run(12345);
        check("E2: equal seed → identical periodic total (deterministic)", a == b);
        check("E2: 4 ranged ticks within [4,400]", a >= 4 && a <= 400);

        // E3 — clear() drops everything and zeroes stat deltas.
        Unit u3 = make_unit(100);
        AuraContainer c3(u3);
        Ability buff = make_aura_ability(5003, 5000);
        buff.effects[0].stat_mods.push_back(StatMod{StatKey::kIntellect, +9});
        c3.apply(buff, 0, 7);
        check("E3: intellect +9 before clear", c3.stat_delta(StatKey::kIntellect) == 9);
        c3.clear();
        check("E3: empty after clear", c3.empty());
        check("E3: intellect delta 0 after clear", c3.stat_delta(StatKey::kIntellect) == 0);

        // E4 — a non-aura effect is rejected (guard).
        Unit u4 = make_unit(100);
        AuraContainer c4(u4);
        Ability strike;
        strike.id = 5004;
        AbilityEffect dmg;
        dmg.kind = EffectKind::kDamage;
        strike.effects.push_back(dmg);
        AuraApplyResult rej = c4.apply(strike, 0, 7);
        check("E4: non-aura effect rejected", rej.action == AuraApplyAction::kRejected);
        check("E4: nothing added", c4.empty());
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD AURA CONTAINER TESTS PASSED\n"
                            : "\n%d WORLDD AURA CONTAINER TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
