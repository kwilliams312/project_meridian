// SPDX-License-Identifier: Apache-2.0
//
// worldd — compiled ability/effect data model + store implementation (CMB-01
// foundation; issue #343). See ability_store.h for the design + the SAD/DDL
// citations. Clean-room from the SAD + the IF-4 world DDL; no GPL / emulator
// source consulted (CONTRIBUTING.md).

#include "ability_store.h"

namespace meridian::worldd {

// ---------------------------------------------------------------------------
// AbilityStore
// ---------------------------------------------------------------------------

AbilityStore AbilityStore::from_abilities(const std::vector<Ability>& abilities,
                                          AbilityId* duplicate_id_out) {
    AbilityStore store;
    store.by_id_.reserve(abilities.size());
    bool reported = false;
    for (const Ability& a : abilities) {
        // First-wins on a duplicate id: emplace only inserts when the key is new.
        // A duplicate is a content fault (mcc/IF-9 must assign unique ids); we drop
        // the later row and surface the FIRST offending id rather than throwing, so
        // a bad content pack degrades gracefully instead of crashing worldd's boot
        // (client SAD §2.4 "never crash"). The lookup below is O(1) regardless.
        auto [it, inserted] = store.by_id_.emplace(a.id, a);
        (void)it;
        if (!inserted && !reported) {
            if (duplicate_id_out != nullptr) *duplicate_id_out = a.id;
            reported = true;
        }
    }
    return store;
}

const Ability* AbilityStore::find(AbilityId id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &it->second;
}

bool AbilityStore::contains(AbilityId id) const {
    return by_id_.find(id) != by_id_.end();
}

// ---------------------------------------------------------------------------
// Placeholder content (M1 seam — issue #343). These are DEV STAND-INS, not real
// content: obviously-synthetic ids in the reserved 0xF000_0000 band, round tuning
// numbers. Epic #28 (mcc v1) replaces this whole function with a world-DB read
// that produces the SAME std::vector<Ability>. Do NOT author real content here.
// ---------------------------------------------------------------------------

namespace {

// A single-effect direct-damage ability builder (melee strike / nuke shape).
Ability make_damage_ability(AbilityId id, const char* name, School school,
                            TargetKind target, float range_m,
                            std::uint32_t cast_time_ms, AbilityResourceType res_type,
                            std::uint32_t res_amount, std::uint32_t dmg_min,
                            std::uint32_t dmg_max) {
    Ability a;
    a.id = id;
    a.name = name;
    a.school = school;
    a.target = target;
    a.range_m = range_m;
    a.cast_time_ms = cast_time_ms;
    a.triggers_gcd = true;
    a.resource_type = res_type;
    a.resource_amount = res_amount;

    AbilityEffect e;
    e.kind = EffectKind::kDamage;
    e.amount_min = dmg_min;
    e.amount_max = dmg_max;
    e.coefficient = 0.0f;  // M2 tuning (SAD/ability.schema.yaml)
    a.effects.push_back(e);
    return a;
}

}  // namespace

std::vector<Ability> placeholder_ability_set() {
    std::vector<Ability> set;
    set.reserve(4);

    // 1) Melee strike — instant, melee range (5 m), physical, free, direct damage.
    //    The resolver's simplest path: no cast timer, no resource, no GCD subtlety.
    set.push_back(make_damage_ability(
        kPlaceholderMeleeStrikeId, "Placeholder Melee Strike", School::kPhysical,
        TargetKind::kEnemy, /*range_m=*/5.0f, /*cast_time_ms=*/0,
        AbilityResourceType::kNone, /*res_amount=*/0, /*dmg_min=*/8, /*dmg_max=*/12));

    // 2) Nuke — 1.5 s cast, 30 m ranged, fire, mana cost, direct damage.
    //    Exercises the cast-timer + resource + range validate path (§3.3).
    set.push_back(make_damage_ability(
        kPlaceholderNukeId, "Placeholder Fire Nuke", School::kFire,
        TargetKind::kEnemy, /*range_m=*/30.0f, /*cast_time_ms=*/1500,
        AbilityResourceType::kMana, /*res_amount=*/20, /*dmg_min=*/40, /*dmg_max=*/55));

    // 3) Heal — 2.0 s cast, 40 m, friendly target, holy, mana cost, heal effect.
    //    Exercises the friendly-target legality + heal application path.
    {
        Ability heal;
        heal.id = kPlaceholderHealId;
        heal.name = "Placeholder Heal";
        heal.school = School::kHoly;
        heal.target = TargetKind::kFriendly;
        heal.range_m = 40.0f;
        heal.cast_time_ms = 2000;
        heal.triggers_gcd = true;
        heal.resource_type = AbilityResourceType::kMana;
        heal.resource_amount = 25;
        AbilityEffect e;
        e.kind = EffectKind::kHeal;
        e.amount_min = 50;
        e.amount_max = 65;
        e.coefficient = 0.0f;
        heal.effects.push_back(e);
        set.push_back(std::move(heal));
    }

    // 4) DoT — instant apply, 30 m, enemy, shadow, mana cost, an aura effect whose
    //    periodic ticks deal damage. Exercises the aura container + periodic path
    //    (the resolver's aura story) with a small, deterministic tick.
    {
        Ability dot;
        dot.id = kPlaceholderDotId;
        dot.name = "Placeholder Shadow DoT";
        dot.school = School::kShadow;
        dot.target = TargetKind::kEnemy;
        dot.range_m = 30.0f;
        dot.cast_time_ms = 0;  // instant apply
        dot.triggers_gcd = true;
        dot.resource_type = AbilityResourceType::kMana;
        dot.resource_amount = 15;
        AbilityEffect aura;
        aura.kind = EffectKind::kAura;
        aura.duration_ms = 12000;  // 12 s
        aura.max_stacks = 1;
        aura.periodic_kind = PeriodicKind::kDamage;
        aura.periodic_amount_min = 6;
        aura.periodic_amount_max = 9;
        aura.periodic_tick_ms = 3000;  // ticks every 3 s -> 4 ticks over 12 s
        dot.effects.push_back(std::move(aura));
        set.push_back(std::move(dot));
    }

    return set;
}

AbilityStore load_placeholder_ability_store() {
    return AbilityStore::from_abilities(placeholder_ability_set());
}

// ---------------------------------------------------------------------------
// Enum name helpers (logs / tooling / diagnostics — not the hot path)
// ---------------------------------------------------------------------------

const char* school_name(School s) {
    switch (s) {
        case School::kPhysical: return "physical";
        case School::kFire:     return "fire";
        case School::kFrost:    return "frost";
        case School::kNature:   return "nature";
        case School::kShadow:   return "shadow";
        case School::kHoly:     return "holy";
        case School::kArcane:   return "arcane";
    }
    return "unknown";
}

const char* target_kind_name(TargetKind t) {
    switch (t) {
        case TargetKind::kSelf:     return "self";
        case TargetKind::kEnemy:    return "enemy";
        case TargetKind::kFriendly: return "friendly";
    }
    return "unknown";
}

const char* resource_type_name(AbilityResourceType r) {
    switch (r) {
        case AbilityResourceType::kNone:   return "none";
        case AbilityResourceType::kMana:   return "mana";
        case AbilityResourceType::kRage:   return "rage";
        case AbilityResourceType::kEnergy: return "energy";
    }
    return "unknown";
}

const char* effect_kind_name(EffectKind k) {
    switch (k) {
        case EffectKind::kDamage:   return "damage";
        case EffectKind::kHeal:     return "heal";
        case EffectKind::kAura:     return "aura";
        case EffectKind::kThreat:   return "threat";
        case EffectKind::kDot:      return "dot";
        case EffectKind::kHot:      return "hot";
        case EffectKind::kBuff:     return "buff";
        case EffectKind::kDebuff:   return "debuff";
        case EffectKind::kShield:   return "shield";
        case EffectKind::kCc:       return "cc";
        case EffectKind::kResource: return "resource";
        case EffectKind::kMovement: return "movement";
        case EffectKind::kSummon:   return "summon";
    }
    return "unknown";
}

const char* attribute_modifier_name(AttributeModifier m) {
    switch (m) {
        case AttributeModifier::kFlat:    return "flat";
        case AttributeModifier::kPercent: return "percent";
    }
    return "unknown";
}

const char* crowd_control_kind_name(CrowdControlKind c) {
    switch (c) {
        case CrowdControlKind::kStun:    return "stun";
        case CrowdControlKind::kRoot:    return "root";
        case CrowdControlKind::kSilence: return "silence";
    }
    return "unknown";
}

const char* resource_pool_name(ResourcePool p) {
    switch (p) {
        case ResourcePool::kMana:   return "mana";
        case ResourcePool::kRage:   return "rage";
        case ResourcePool::kEnergy: return "energy";
    }
    return "unknown";
}

const char* resource_op_name(ResourceOp o) {
    switch (o) {
        case ResourceOp::kGrant: return "grant";
        case ResourceOp::kDrain: return "drain";
    }
    return "unknown";
}

const char* movement_motion_name(MovementMotion m) {
    switch (m) {
        case MovementMotion::kKnockback: return "knockback";
        case MovementMotion::kPull:      return "pull";
        case MovementMotion::kDash:      return "dash";
    }
    return "unknown";
}

}  // namespace meridian::worldd
