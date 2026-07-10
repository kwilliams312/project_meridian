// SPDX-License-Identifier: Apache-2.0
//
// worldd — server-authoritative combat resolver (issues #344 + #345, CMB-01).
// See combat_resolver.h for the design + clean-room provenance.

#include "combat_resolver.h"

#include <algorithm>
#include <cmath>

#include "aoi_grid.h"  // horizontal_distance

namespace meridian::worldd {

// ---------------------------------------------------------------------------
// Faction relations (the resolver's call — #342 Unit header).
// ---------------------------------------------------------------------------
bool can_attack(Faction attacker, Faction target) {
    switch (attacker) {
        case Faction::kPlayer:
            // Players harm hostile creatures only.
            return target == Faction::kHostile;
        case Faction::kHostile:
            // Hostile creatures harm players and friendly NPCs.
            return target == Faction::kPlayer || target == Faction::kFriendly;
        case Faction::kFriendly:
        case Faction::kNeutral:
            // Friendly NPCs and neutral entities initiate no harm at M1.
            return false;
    }
    return false;
}

bool can_assist(Faction caster, Faction target) {
    switch (caster) {
        case Faction::kPlayer:
        case Faction::kFriendly:
            // Player-aligned units aid each other.
            return target == Faction::kPlayer || target == Faction::kFriendly;
        case Faction::kHostile:
            // Hostiles aid hostiles.
            return target == Faction::kHostile;
        case Faction::kNeutral:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Line of sight — flat bootstrap map has no occluders (D-19).
// ---------------------------------------------------------------------------
bool flat_map_los(const Position& /*from*/, const Position& /*to*/) { return true; }

// ---------------------------------------------------------------------------
// Attack-table classifiers (pure — testable at each band boundary).
// ---------------------------------------------------------------------------
AttackOutcome classify_attack(std::uint32_t roll_bp) {
    std::uint32_t threshold = kBaseMissBp;
    if (roll_bp < threshold) return AttackOutcome::kMiss;
    threshold += kBaseDodgeBp;
    if (roll_bp < threshold) return AttackOutcome::kDodge;
    threshold += kBaseParryBp;
    if (roll_bp < threshold) return AttackOutcome::kParry;
    threshold += kBaseCritBp;
    if (roll_bp < threshold) return AttackOutcome::kCrit;
    return AttackOutcome::kHit;
}

AttackOutcome classify_heal(std::uint32_t roll_bp) {
    return roll_bp < kHealCritBp ? AttackOutcome::kCrit : AttackOutcome::kHit;
}

bool is_heal_ability(const Ability& ability) {
    bool has_heal = false;
    for (const AbilityEffect& e : ability.effects) {
        if (e.kind == EffectKind::kDamage) return false;  // any direct damage => attack
        if (e.kind == EffectKind::kHeal) has_heal = true;
    }
    return has_heal;
}

AttackOutcome roll_attack(const Ability& ability, CombatRng& rng) {
    const std::uint32_t r = rng.roll_bp();
    return is_heal_ability(ability) ? classify_heal(r) : classify_attack(r);
}

// ---------------------------------------------------------------------------
// Target / range / LoS validation (#345, SAD §3.3).
// ---------------------------------------------------------------------------
CastReject validate_target(const Ability& ability, const Unit& caster,
                           const Unit* target, const LineOfSightFn& los) {
    // A self ability needs no external target — legal for a live caster.
    if (ability.target == TargetKind::kSelf) return CastReject::kNone;

    if (target == nullptr) return CastReject::kNoTarget;
    if (target->is_dead()) return CastReject::kTargetDead;

    if (ability.target == TargetKind::kEnemy) {
        if (!can_attack(caster.faction(), target->faction()))
            return CastReject::kWrongFaction;
    } else {  // kFriendly
        if (!can_assist(caster.faction(), target->faction()))
            return CastReject::kWrongFaction;
    }

    const float dist = horizontal_distance(caster.position(), target->position());
    if (dist > ability.range_m) return CastReject::kOutOfRange;

    if (los && !los(caster.position(), target->position()))
        return CastReject::kNoLineOfSight;

    return CastReject::kNone;
}

// ---------------------------------------------------------------------------
// Apply a rolled outcome to the target (#345).
// ---------------------------------------------------------------------------
namespace {

// Scale a base amount by the crit multiplier (rounded to nearest).
std::uint32_t apply_crit(std::uint32_t base) {
    return static_cast<std::uint32_t>(std::lround(static_cast<double>(base) * kCritMultiplier));
}

}  // namespace

ResolveResult apply_outcome(const Ability& ability, Unit& /*caster*/, Unit& target,
                            AttackOutcome outcome, CombatRng& rng) {
    ResolveResult res;
    res.outcome = outcome;
    res.is_heal = is_heal_ability(ability);

    const bool avoided = (outcome == AttackOutcome::kMiss ||
                          outcome == AttackOutcome::kDodge ||
                          outcome == AttackOutcome::kParry);
    const bool crit = (outcome == AttackOutcome::kCrit);

    std::uint32_t total_applied = 0;
    bool died = false;

    for (const AbilityEffect& e : ability.effects) {
        if (e.kind == EffectKind::kDamage) {
            if (avoided) continue;  // miss/dodge/parry — nothing lands
            std::uint32_t amount = rng.roll_amount(e.amount_min, e.amount_max);
            if (crit) amount = apply_crit(amount);
            DamageResult dr = target.apply_damage(amount);
            total_applied += dr.applied;
            died = died || dr.lethal;
        } else if (e.kind == EffectKind::kHeal) {
            // Heals are never avoided (classify_heal only yields hit/crit).
            std::uint32_t amount = rng.roll_amount(e.amount_min, e.amount_max);
            if (crit) amount = apply_crit(amount);
            total_applied += target.apply_healing(amount);
        }
        // kAura (#346) and kThreat (CMB-04) are intentionally skipped here.
    }

    res.amount = total_applied;
    res.target_died = died;
    res.target_health = target.health();
    return res;
}

ResolveResult resolve_ability(const Ability& ability, Unit& caster, Unit& target,
                              CombatRng& rng) {
    const AttackOutcome outcome = roll_attack(ability, rng);
    return apply_outcome(ability, caster, target, outcome, rng);
}

// ---------------------------------------------------------------------------
// #344 — GCD + cast lifecycle accept/reject decision.
// ---------------------------------------------------------------------------
CastDecision begin_ability_use(CombatSession& combat, const Ability& ability,
                               Unit& caster, const Unit* target,
                               ObjectGuid target_guid, const LineOfSightFn& los,
                               std::uint64_t now_ms) {
    CastDecision d;

    auto reject = [&](CastReject why) {
        d.accepted = false;
        d.reject = why;
        d.gcd_remaining_ms = combat.gcd_remaining_ms(now_ms);
        return d;
    };

    // SAD §3.3 validate order: caster alive, GCD clock, in-progress cast,
    // resource, then target legality/range/LoS.
    if (caster.is_dead()) return reject(CastReject::kCasterDead);
    if (ability.triggers_gcd && combat.on_gcd(now_ms)) return reject(CastReject::kOnGcd);
    if (combat.is_casting(now_ms)) return reject(CastReject::kAlreadyCasting);

    // Resource: an all-or-nothing check against the caster's secondary pool. We
    // compare the ability cost to the caster's current resource; the pool TYPE
    // match is a later concern (the placeholder players carry a single pool). A
    // free ability (resource_amount == 0) always passes.
    if (ability.resource_amount > 0 && caster.resource() < ability.resource_amount)
        return reject(CastReject::kInsufficientResource);

    const CastReject tgt = validate_target(ability, caster, target, los);
    if (tgt != CastReject::kNone) return reject(tgt);

    // Accepted — start the lifecycle. Trigger the GCD (if this ability triggers
    // it), then either resolve instantly or arm the cast timer.
    d.accepted = true;
    d.reject = CastReject::kNone;
    if (ability.triggers_gcd) combat.trigger_gcd(now_ms);

    d.cast_ms = ability.cast_time_ms;
    if (ability.cast_time_ms == 0) {
        d.instant = true;
    } else {
        d.instant = false;
        PendingCast pc;
        pc.ability_id = ability.id;
        pc.target_guid = target_guid;
        pc.cast_time_ms = ability.cast_time_ms;
        pc.cast_end_ms = now_ms + ability.cast_time_ms;
        combat.begin_cast(pc);
    }
    return d;
}

}  // namespace meridian::worldd
