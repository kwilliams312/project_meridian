// SPDX-License-Identifier: Apache-2.0
//
// worldd — combat resolver UNIT TEST (issues #344 + #345, CMB-01).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5/§3.3 (combat resolver +
// the CastRequest validate list), client SAD §2.2/§3c (D-10 accept/reject), and
// combat_resolver.h / combat_unit.h / ability_store.h only. No GPL / emulator
// source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: exercises the resolver core directly with a
// SEEDED RNG, so every roll is deterministic (SAD §2.5 "seeded-RNG unit-
// testable"). Runs in the plain server ctest.
//
// What it proves:
//   A. ATTACK-TABLE classifiers — each band (miss/dodge/parry/crit/hit) at its
//      boundary; the heal table has no avoidance.
//   B. is_heal_ability — heal vs damage vs aura-only discrimination.
//   C. FACTION relations — can_attack / can_assist matrix.
//   D. TARGET VALIDATION — legal target vs no-target / dead / wrong-faction /
//      out-of-range / no-LoS; a self ability needs no target.
//   E. RESOLUTION application — HIT/CRIT apply (and double) damage; miss/dodge/
//      parry apply nothing; heals restore HP; DEATH triggers at 0 HP.
//   F. SEEDED RNG — every attack-table outcome is reachable from the real roll
//      stream; the same seed yields the same sequence (reproducible).
//   G. GCD + CAST lifecycle (#344) — accept/reject decisions in SAD §3.3 order;
//      GCD enforcement; instant vs cast-time; already-casting; insufficient
//      resource; caster-dead; a reject leaves the session state untouched.
//   H. INTERRUPT + PUSHBACK — interrupt cancels a cast; pushback delays its
//      completion; take_completed fires exactly at/after the end time.

#include "combat_resolver.h"

#include "ability_store.h"
#include "combat_unit.h"
#include "movement_validation.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

using namespace meridian::worldd;

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
    p.z = 0.0f;
    return p;
}

// A damage ability with a FIXED amount (amount_min == amount_max) so application
// is deterministic regardless of the RNG amount roll.
Ability make_damage(std::uint32_t amount, float range = 100.0f,
                    std::uint32_t cast_ms = 0, std::uint32_t cost = 0,
                    bool triggers_gcd = true) {
    Ability a;
    a.id = 1001;
    a.name = "test-damage";
    a.school = School::kPhysical;
    a.target = TargetKind::kEnemy;
    a.range_m = range;
    a.cast_time_ms = cast_ms;
    a.triggers_gcd = triggers_gcd;
    a.resource_amount = cost;
    a.resource_type = cost > 0 ? AbilityResourceType::kMana : AbilityResourceType::kNone;
    AbilityEffect e;
    e.kind = EffectKind::kDamage;
    e.amount_min = amount;
    e.amount_max = amount;
    a.effects.push_back(e);
    return a;
}

Ability make_heal(std::uint32_t amount, float range = 100.0f) {
    Ability a;
    a.id = 1002;
    a.name = "test-heal";
    a.school = School::kHoly;
    a.target = TargetKind::kFriendly;
    a.range_m = range;
    AbilityEffect e;
    e.kind = EffectKind::kHeal;
    e.amount_min = amount;
    e.amount_max = amount;
    a.effects.push_back(e);
    return a;
}

// An aura-only ability (a DoT shape) — no direct damage/heal effect.
Ability make_aura_only() {
    Ability a;
    a.id = 1003;
    a.name = "test-dot";
    a.target = TargetKind::kEnemy;
    AbilityEffect e;
    e.kind = EffectKind::kAura;
    e.periodic_kind = PeriodicKind::kDamage;
    e.periodic_amount_min = 6;
    e.periodic_amount_max = 9;
    a.effects.push_back(e);
    return a;
}

Player make_caster(std::uint32_t hp = 200, std::uint32_t mana = 100) {
    UnitStats s;
    s.level = 1;
    s.max_health = hp;
    s.faction = Faction::kPlayer;
    s.resource_type = ResourceType::kMana;
    s.max_resource = mana;
    return Player(1, at(0, 0), s, /*account=*/1, /*char_class=*/2, "Caster");
}

Creature make_hostile(std::uint32_t hp, float x, float y) {
    UnitStats s;
    s.level = 1;
    s.max_health = hp;
    s.faction = Faction::kHostile;
    return Creature(2, at(x, y), s, /*template_id=*/1);
}

}  // namespace

int main() {
    std::printf("worldd combat-resolver unit test\n");

    // === A. attack-table classifiers ========================================
    // Bands: [0,500) miss, [500,1000) dodge, [1000,1500) parry, [1500,2500) crit,
    // [2500,10000) hit.
    check("A: roll 0 -> miss",       classify_attack(0) == AttackOutcome::kMiss);
    check("A: roll 499 -> miss",     classify_attack(499) == AttackOutcome::kMiss);
    check("A: roll 500 -> dodge",    classify_attack(500) == AttackOutcome::kDodge);
    check("A: roll 999 -> dodge",    classify_attack(999) == AttackOutcome::kDodge);
    check("A: roll 1000 -> parry",   classify_attack(1000) == AttackOutcome::kParry);
    check("A: roll 1499 -> parry",   classify_attack(1499) == AttackOutcome::kParry);
    check("A: roll 1500 -> crit",    classify_attack(1500) == AttackOutcome::kCrit);
    check("A: roll 2499 -> crit",    classify_attack(2499) == AttackOutcome::kCrit);
    check("A: roll 2500 -> hit",     classify_attack(2500) == AttackOutcome::kHit);
    check("A: roll 9999 -> hit",     classify_attack(9999) == AttackOutcome::kHit);
    // Heal table: [0,1000) crit, else hit — never avoided.
    check("A: heal roll 0 -> crit",    classify_heal(0) == AttackOutcome::kCrit);
    check("A: heal roll 999 -> crit",  classify_heal(999) == AttackOutcome::kCrit);
    check("A: heal roll 1000 -> hit",  classify_heal(1000) == AttackOutcome::kHit);

    // === B. is_heal_ability =================================================
    check("B: heal ability is a heal", is_heal_ability(make_heal(50)));
    check("B: damage ability is not a heal", !is_heal_ability(make_damage(10)));
    check("B: aura-only ability is not a heal", !is_heal_ability(make_aura_only()));

    // === C. faction relations ===============================================
    check("C: player attacks hostile", can_attack(Faction::kPlayer, Faction::kHostile));
    check("C: player cannot attack player", !can_attack(Faction::kPlayer, Faction::kPlayer));
    check("C: hostile attacks player", can_attack(Faction::kHostile, Faction::kPlayer));
    check("C: neutral attacks no one", !can_attack(Faction::kPlayer, Faction::kNeutral));
    check("C: player assists player", can_assist(Faction::kPlayer, Faction::kPlayer));
    check("C: player assists friendly", can_assist(Faction::kPlayer, Faction::kFriendly));
    check("C: player cannot assist hostile", !can_assist(Faction::kPlayer, Faction::kHostile));

    // === D. target validation ===============================================
    {
        Player caster = make_caster();
        Creature enemy = make_hostile(100, 3, 0);  // 3 m away
        Ability dmg = make_damage(10, /*range=*/5.0f);

        check("D: legal enemy in range -> kNone",
              validate_target(dmg, caster, &enemy, flat_map_los) == CastReject::kNone);
        check("D: null target -> kNoTarget",
              validate_target(dmg, caster, nullptr, flat_map_los) == CastReject::kNoTarget);

        Creature dead = make_hostile(100, 3, 0);
        dead.kill();
        check("D: dead target -> kTargetDead",
              validate_target(dmg, caster, &dead, flat_map_los) == CastReject::kTargetDead);

        // A friendly player is not a legal ENEMY target.
        Player friendly = make_caster();
        check("D: friendly as enemy target -> kWrongFaction",
              validate_target(dmg, caster, &friendly, flat_map_los) == CastReject::kWrongFaction);

        Creature far = make_hostile(100, 50, 0);  // 50 m, ability range 5
        check("D: out of range -> kOutOfRange",
              validate_target(dmg, caster, &far, flat_map_los) == CastReject::kOutOfRange);

        // Blocked line of sight (injected occluder).
        LineOfSightFn blocked = [](const Position&, const Position&) { return false; };
        check("D: blocked LoS -> kNoLineOfSight",
              validate_target(dmg, caster, &enemy, blocked) == CastReject::kNoLineOfSight);

        // A friendly heal on a friendly target is legal.
        Ability heal = make_heal(50, /*range=*/40.0f);
        check("D: heal on friendly -> kNone",
              validate_target(heal, caster, &friendly, flat_map_los) == CastReject::kNone);

        // A self ability needs no target.
        Ability self_ab = make_heal(50);
        self_ab.target = TargetKind::kSelf;
        check("D: self ability ignores target -> kNone",
              validate_target(self_ab, caster, nullptr, flat_map_los) == CastReject::kNone);
    }

    // === E. resolution application (explicit outcomes) ======================
    {
        CombatRng rng(1);
        Ability dmg = make_damage(10);

        Player caster = make_caster();
        Creature t_hit = make_hostile(100, 0, 0);
        ResolveResult hit = apply_outcome(dmg, caster, t_hit, AttackOutcome::kHit, rng);
        check("E: HIT applies exact damage", hit.amount == 10 && t_hit.health() == 90 &&
                                                 !hit.target_died && !hit.is_heal);

        Creature t_crit = make_hostile(100, 0, 0);
        ResolveResult crit = apply_outcome(dmg, caster, t_crit, AttackOutcome::kCrit, rng);
        check("E: CRIT doubles damage", crit.amount == 20 && t_crit.health() == 80);

        Creature t_miss = make_hostile(100, 0, 0);
        ResolveResult miss = apply_outcome(dmg, caster, t_miss, AttackOutcome::kMiss, rng);
        check("E: MISS applies nothing", miss.amount == 0 && t_miss.health() == 100);
        ResolveResult dodge = apply_outcome(dmg, caster, t_miss, AttackOutcome::kDodge, rng);
        check("E: DODGE applies nothing", dodge.amount == 0 && t_miss.health() == 100);
        ResolveResult parry = apply_outcome(dmg, caster, t_miss, AttackOutcome::kParry, rng);
        check("E: PARRY applies nothing", parry.amount == 0 && t_miss.health() == 100);

        // Death: a HIT that exceeds remaining HP kills.
        Ability big = make_damage(50);
        Creature t_die = make_hostile(40, 0, 0);
        ResolveResult killed = apply_outcome(big, caster, t_die, AttackOutcome::kHit, rng);
        check("E: lethal HIT triggers death",
              killed.target_died && t_die.is_dead() && t_die.health() == 0 &&
                  killed.amount == 40);  // clamped to remaining HP

        // Heal restores HP (clamped to max), never avoided.
        Ability heal = make_heal(30);
        Player wounded = make_caster();
        wounded.apply_damage(50);  // 200 -> 150
        ResolveResult h = apply_outcome(heal, caster, wounded, AttackOutcome::kHit, rng);
        check("E: HIT heal restores HP",
              h.is_heal && h.amount == 30 && wounded.health() == 180 && !h.target_died);
        ResolveResult hc = apply_outcome(heal, caster, wounded, AttackOutcome::kCrit, rng);
        check("E: CRIT heal doubles (clamped to max)",
              hc.is_heal && wounded.health() == 200);  // 180 + 60 clamped to 200
    }

    // === E2. approved armor contract + basic attacks =======================
    {
        check("E2: zero armor leaves physical damage unchanged",
              mitigate_damage(100, School::kPhysical, 0) == 100);
        check("E2: 100 armor halves physical damage",
              mitigate_damage(100, School::kPhysical, 100) == 50);
        check("E2: physical mitigation floors integer division",
              mitigate_damage(10, School::kPhysical, 50) == 6);
        check("E2: non-zero physical hits have a one-damage floor",
              mitigate_damage(1, School::kPhysical, 2'000'000'000) == 1);
        check("E2: negative effective armor clamps to zero",
              mitigate_damage(100, School::kPhysical, -50) == 100);
        check("E2: non-physical schools bypass armor",
              mitigate_damage(100, School::kFire, 1000) == 100);

        Player caster = make_caster();
        UnitStats armored_stats;
        armored_stats.max_health = 200;
        armored_stats.armor = 100;
        armored_stats.faction = Faction::kHostile;
        Creature armored(2, at(0, 0), armored_stats, 1);
        armored.add_absorb(20);
        CombatRng rng(1);
        ResolveResult physical = apply_outcome(
            make_damage(100), caster, armored, AttackOutcome::kHit, rng);
        check("E2: armor applies before shields then health",
              physical.absorbed == 20 && physical.amount == 30 &&
                  armored.absorb() == 0 && armored.health() == 170);

        Ability fire = make_damage(100);
        fire.school = School::kFire;
        Creature elemental(3, at(0, 0), armored_stats, 1);
        ResolveResult magic = apply_outcome(
            fire, caster, elemental, AttackOutcome::kHit, rng);
        check("E2: non-physical ability bypasses target armor",
              magic.amount == 100 && elemental.health() == 100);

        // Full basic-attack path: same seed means the same outcome/raw/applied
        // result, and the target's effective armor is honored.
        UnitStats player_stats;
        player_stats.max_health = 500;
        player_stats.armor = 100;
        player_stats.faction = Faction::kPlayer;
        Player ta(10, at(0, 0), player_stats, 1, 1, "A");
        Player tb(11, at(0, 0), player_stats, 1, 1, "B");
        UnitStats mob_stats;
        mob_stats.max_health = 100;
        mob_stats.faction = Faction::kHostile;
        Creature ma(20, at(0, 0), mob_stats, 1);
        Creature mb(21, at(0, 0), mob_stats, 1);
        CombatRng ra(784), rb(784);
        BasicAttackResult ba = resolve_basic_attack(ma, ta, 5, 8, ra);
        BasicAttackResult bb = resolve_basic_attack(mb, tb, 5, 8, rb);
        check("E2: seeded basic attacks are deterministic",
              ba.outcome == bb.outcome && ba.raw_amount == bb.raw_amount &&
                  ba.amount == bb.amount && ta.health() == tb.health());
    }

    // === F. seeded RNG: every outcome reachable + reproducible ==============
    {
        Ability dmg = make_damage(10);
        CombatRng rng(12345);
        std::map<AttackOutcome, int> counts;
        for (int i = 0; i < 20000; ++i) counts[roll_attack(dmg, rng)]++;
        check("F: MISS occurs",  counts[AttackOutcome::kMiss] > 0);
        check("F: DODGE occurs", counts[AttackOutcome::kDodge] > 0);
        check("F: PARRY occurs", counts[AttackOutcome::kParry] > 0);
        check("F: CRIT occurs",  counts[AttackOutcome::kCrit] > 0);
        check("F: HIT occurs",   counts[AttackOutcome::kHit] > 0);

        Ability heal = make_heal(10);
        CombatRng hrng(777);
        int heal_crit = 0, heal_hit = 0, heal_other = 0;
        for (int i = 0; i < 5000; ++i) {
            AttackOutcome o = roll_attack(heal, hrng);
            if (o == AttackOutcome::kCrit) ++heal_crit;
            else if (o == AttackOutcome::kHit) ++heal_hit;
            else ++heal_other;
        }
        check("F: heal never avoided", heal_other == 0 && heal_crit > 0 && heal_hit > 0);

        // Reproducibility: same seed -> same roll sequence.
        CombatRng a(42), b(42);
        bool same = true;
        for (int i = 0; i < 1000; ++i) same = same && (a.roll_bp() == b.roll_bp());
        check("F: same seed yields same sequence", same);
        CombatRng c(43);
        CombatRng d(42);
        bool differ = false;
        for (int i = 0; i < 1000 && !differ; ++i) differ = (c.roll_bp() != d.roll_bp());
        check("F: different seeds diverge", differ);
    }

    // === G. GCD + cast lifecycle (accept/reject decisions) ==================
    {
        // Instant melee-style, no resource cost, enemy in range.
        Ability instant = make_damage(10, /*range=*/5.0f, /*cast_ms=*/0);
        Creature enemy = make_hostile(100, 3, 0);

        {
            CombatSession cs;
            Player caster = make_caster();
            CastDecision d = begin_ability_use(cs, instant, caster, &enemy, 2, flat_map_los,
                                               /*now=*/1000);
            check("G: instant enemy accepted",
                  d.accepted && d.instant && d.cast_ms == 0 && d.reject == CastReject::kNone);
            check("G: accept started the GCD", cs.on_gcd(1000));

            // Second use during the GCD is rejected with the remaining time.
            CastDecision d2 = begin_ability_use(cs, instant, caster, &enemy, 2, flat_map_los,
                                                /*now=*/1000);
            check("G: on-GCD rejected",
                  !d2.accepted && d2.reject == CastReject::kOnGcd &&
                      d2.gcd_remaining_ms == kGlobalCooldownMs);

            // After the GCD elapses it is castable again.
            CastDecision d3 = begin_ability_use(cs, instant, caster, &enemy, 2, flat_map_los,
                                                /*now=*/1000 + kGlobalCooldownMs);
            check("G: castable again after GCD", d3.accepted);
        }

        // Cast-time ability: accept arms a cast timer, not an instant resolve. Use
        // a cast (3000 ms) LONGER than the GCD (1500 ms) so a second use after the
        // GCD elapses exercises the already-casting branch (not the GCD branch,
        // which the SAD §3.3 order checks first).
        {
            CombatSession cs;
            Player caster = make_caster();
            Ability cast = make_damage(40, /*range=*/30.0f, /*cast_ms=*/3000, /*cost=*/20);
            CastDecision d = begin_ability_use(cs, cast, caster, &enemy, 2, flat_map_los,
                                               /*now=*/0);
            check("G: cast-time accepted, not instant",
                  d.accepted && !d.instant && d.cast_ms == 3000);
            check("G: cast timer armed", cs.is_casting(0) && cs.pending().has_value());

            // A GCD-triggering use DURING the GCD is rejected as on-GCD first.
            CastDecision dg = begin_ability_use(cs, instant, caster, &enemy, 2, flat_map_los,
                                                /*now=*/500);
            check("G: on-GCD checked before casting", dg.reject == CastReject::kOnGcd);

            // After the GCD elapses but while STILL casting -> already-casting.
            CastDecision d2 = begin_ability_use(cs, instant, caster, &enemy, 2, flat_map_los,
                                                /*now=*/2000);
            check("G: already-casting rejected",
                  !d2.accepted && d2.reject == CastReject::kAlreadyCasting);
        }

        // Insufficient resource.
        {
            CombatSession cs;
            Player caster = make_caster(200, /*mana=*/100);
            caster.spend_resource(95);  // 5 mana left
            Ability cast = make_damage(40, 30.0f, 1500, /*cost=*/20);
            CastDecision d = begin_ability_use(cs, cast, caster, &enemy, 2, flat_map_los, 0);
            check("G: insufficient resource rejected",
                  !d.accepted && d.reject == CastReject::kInsufficientResource);
            check("G: reject did NOT start a GCD", !cs.on_gcd(0));
        }

        // Dead caster cannot cast.
        {
            CombatSession cs;
            Player caster = make_caster();
            caster.kill();
            CastDecision d = begin_ability_use(cs, instant, caster, &enemy, 2, flat_map_los, 0);
            check("G: dead caster rejected",
                  !d.accepted && d.reject == CastReject::kCasterDead);
        }

        // Out-of-range target rejected (validation runs inside begin_ability_use).
        {
            CombatSession cs;
            Player caster = make_caster();
            Creature far = make_hostile(100, 50, 0);
            CastDecision d = begin_ability_use(cs, instant, caster, &far, 2, flat_map_los, 0);
            check("G: out-of-range rejected",
                  !d.accepted && d.reject == CastReject::kOutOfRange);
            check("G: target reject did NOT start a GCD", !cs.on_gcd(0));
        }
    }

    // === H. interrupt + pushback ============================================
    {
        // Interrupt cancels an in-progress cast.
        {
            CombatSession cs;
            PendingCast pc;
            pc.ability_id = 7;
            pc.cast_end_ms = 2000;
            pc.cast_time_ms = 2000;
            cs.begin_cast(pc);
            check("H: casting before interrupt", cs.is_casting(500));
            cs.interrupt();
            check("H: interrupt cancels the cast",
                  !cs.is_casting(500) && !cs.pending().has_value());
        }

        // Pushback delays completion; take_completed fires at/after the end.
        {
            CombatSession cs;
            PendingCast pc;
            pc.ability_id = 7;
            pc.cast_end_ms = 2000;
            pc.cast_time_ms = 2000;
            cs.begin_cast(pc);
            check("H: not complete before end", !cs.take_completed(1999).has_value());
            cs.apply_pushback(kCastPushbackMs);  // end -> 2500
            check("H: pushback delays completion", !cs.take_completed(2000).has_value());
            auto done = cs.take_completed(2500);
            check("H: completes at/after pushed-back end",
                  done.has_value() && done->ability_id == 7);
            check("H: completed cast is cleared", !cs.pending().has_value());
        }
    }

    // === (integration) resolve_ability determinism over the full path ======
    {
        Ability dmg = make_damage(10, 100.0f);
        Player caster = make_caster();
        CombatRng r1(999), r2(999);
        bool same = true;
        for (int i = 0; i < 500; ++i) {
            Creature ta = make_hostile(10000, 0, 0);
            Creature tb = make_hostile(10000, 0, 0);
            ResolveResult a = resolve_ability(dmg, caster, ta, r1);
            ResolveResult b = resolve_ability(dmg, caster, tb, r2);
            same = same && a.outcome == b.outcome && a.amount == b.amount &&
                   ta.health() == tb.health();
        }
        check("I: resolve_ability is deterministic for a fixed seed", same);
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD COMBAT-RESOLVER TESTS PASSED\n"
                            : "\n%d WORLDD COMBAT-RESOLVER TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
