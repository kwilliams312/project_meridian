// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-Unit combat aura container (issue #346, CMB-01; extended by issue
// #361, CMB-04 — dispel types + the dispel() operation). Implementation of
// aura_container.h. CLEAN-ROOM from docs/sad/server-sad.md §2.5/§3.2/§9 + the IF-4
// world DDL (schema/sql/world/30_ability.sql) — see the header for the full
// provenance note. No GPL / emulator source consulted (CONTRIBUTING.md).

#include "aura_container.h"

namespace meridian::worldd {

namespace {

// StatKey → net-totals index. StatKey is a contiguous enum (strength..spirit), so
// its underlying value IS the index; this helper documents that + bounds it.
std::size_t stat_index(StatKey stat) {
    const auto i = static_cast<std::size_t>(stat);
    return i < kStatKeyCount ? i : 0;
}

// A kShield's absorb amount = a seeded roll in [amount_min, amount_max] (SP2.3
// #693). With no RNG (null) we take amount_min so the grant is still deterministic.
std::uint32_t roll_shield_amount(const AbilityEffect& e, CombatRng* rng) {
    if (rng == nullptr) return e.amount_min;
    return rng->roll_amount(e.amount_min, e.amount_max);
}

}  // namespace

// ---------------------------------------------------------------------------
// modifier folding + queries
// ---------------------------------------------------------------------------

void AuraContainer::fold_modifiers(const ActiveAura& aura, std::uint16_t stack_count,
                                   int sign) {
    if (stack_count == 0) return;
    const std::int32_t stacks = static_cast<std::int32_t>(stack_count);

    // (a) kAura authored stat_mods → the primary StatKey ledger.
    for (const StatMod& m : aura.stat_mods) {
        // amount is PER STACK: fold `stack_count` stacks' worth, with `sign`.
        stat_totals_[stat_index(m.stat)] += sign * m.amount * stacks;
    }

    // (b) kBuff/kDebuff attribute modifier → the attribute ledger the SP2.4 #694
    //     EffectiveStats framework reads. A PRIMARY flat modifier ALSO mirrors into
    //     stat_totals_, so the StatKey view stays coherent with the aura layer.
    if ((aura.kind == EffectKind::kBuff || aura.kind == EffectKind::kDebuff) &&
        !aura.attr_ref.empty()) {
        const std::int32_t delta = sign * aura.attr_amount * stacks;
        if (aura.attr_modifier == AttributeModifier::kPercent) {
            attr_percent_[aura.attr_ref] += delta;
        } else {
            attr_flat_[aura.attr_ref] += delta;
            StatKey sk;
            if (primary_attribute_stat(aura.attr_ref, sk)) {
                stat_totals_[stat_index(sk)] += delta;
            }
        }
    }

    // (c) kShield absorb → reclaimed from the host on teardown (sign < 0). The grant
    //     (sign > 0) is done explicitly at apply time, where the seeded roll happens.
    if (sign < 0 && aura.is_shield()) {
        host_.remove_absorb(aura.shield_amount);
    }
}

std::int32_t AuraContainer::stat_delta(StatKey stat) const {
    return stat_totals_[stat_index(stat)];
}

AttributeDelta AuraContainer::attribute_delta(const std::string& attribute_ref) const {
    AttributeDelta d;
    if (const auto it = attr_flat_.find(attribute_ref); it != attr_flat_.end())
        d.flat = it->second;
    if (const auto it = attr_percent_.find(attribute_ref); it != attr_percent_.end())
        d.percent = it->second;
    return d;
}

bool AuraContainer::has_control(CrowdControlKind kind) const {
    for (const ActiveAura& a : auras_) {
        if (a.is_control() && a.cc_kind == kind) return true;
    }
    return false;
}

const ActiveAura* AuraContainer::find(AbilityId ability_id, std::uint32_t effect_index,
                                      ObjectGuid caster_guid) const {
    for (const ActiveAura& a : auras_) {
        if (a.ability_id == ability_id && a.effect_index == effect_index &&
            a.caster_guid == caster_guid) {
            return &a;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// apply / refresh / stack
// ---------------------------------------------------------------------------

namespace {

// Whether an effect kind is a TIMED state this container hosts: the classic kAura
// plus the SP2.3 #693 extensions. Instantaneous kinds (damage/heal/threat/resource/
// movement/summon) belong to the resolver / map tick, not here.
bool is_container_kind(EffectKind k) {
    switch (k) {
        case EffectKind::kAura:
        case EffectKind::kDot:
        case EffectKind::kHot:
        case EffectKind::kBuff:
        case EffectKind::kDebuff:
        case EffectKind::kShield:
        case EffectKind::kCc:
            return true;
        default:
            return false;
    }
}

}  // namespace

AuraApplyResult AuraContainer::apply(const Ability& ability, std::uint32_t effect_index,
                                     ObjectGuid caster_guid, DispelType dispel_type,
                                     CombatRng* rng) {
    // Guard: the effect must exist and be a container-hosted timed state. An
    // instantaneous effect (damage/heal/threat/resource/movement/summon) has no
    // place in the container — reject without mutating anything.
    if (effect_index >= ability.effects.size()) {
        return {AuraApplyAction::kRejected, 0};
    }
    const AbilityEffect& e = ability.effects[effect_index];
    if (!is_container_kind(e.kind)) {
        return {AuraApplyAction::kRejected, 0};
    }

    // Existing instance of THIS (ability, effect, caster)? Refresh or stack it.
    for (ActiveAura& a : auras_) {
        if (a.ability_id == ability.id && a.effect_index == effect_index &&
            a.caster_guid == caster_guid) {
            a.remaining_ms = a.duration_ms;  // refresh duration (both rules refresh)
            if (a.max_stacks > 1 && a.stacks < a.max_stacks) {
                // stack-count rule: gain exactly one stack, fold that stack's mods.
                a.stacks = static_cast<std::uint16_t>(a.stacks + 1);
                fold_modifiers(a, 1, +1);
                // A stacking shield grows the absorb pool by another rolled amount.
                if (a.is_shield()) {
                    const std::uint32_t add = roll_shield_amount(e, rng);
                    a.shield_amount += add;
                    host_.add_absorb(add);
                }
                return {AuraApplyAction::kStacked, a.stacks};
            }
            // refresh-duration rule (max_stacks == 1) OR already at the stack cap.
            // A refreshed shield tops its pool back up to a fresh roll.
            if (a.is_shield()) {
                host_.remove_absorb(a.shield_amount);
                a.shield_amount = roll_shield_amount(e, rng);
                host_.add_absorb(a.shield_amount);
            }
            return {AuraApplyAction::kRefreshed, a.stacks};
        }
    }

    // No existing instance — ADD a fresh one. A different caster reaches here even
    // for the same ability effect, giving an INDEPENDENT instance.
    ActiveAura aura;
    aura.ability_id = ability.id;
    aura.effect_index = effect_index;
    aura.caster_guid = caster_guid;
    aura.kind = e.kind;
    aura.max_stacks = e.max_stacks == 0 ? 1 : e.max_stacks;  // clamp a bad 0 to 1
    aura.dispel_type = dispel_type;  // CMB-04 classification, fixed at first apply
    aura.stacks = 1;
    aura.since_last_tick_ms = 0;

    // Per-kind config snapshot (only the fields the kind uses; the rest keep their
    // defaults). Each timed kind maps onto the shared duration/periodic/stat state.
    switch (e.kind) {
        case EffectKind::kAura:
            aura.duration_ms = e.duration_ms;
            aura.periodic_kind = e.periodic_kind;
            aura.periodic_amount_min = e.periodic_amount_min;
            aura.periodic_amount_max = e.periodic_amount_max;
            aura.periodic_tick_ms = e.periodic_tick_ms;
            aura.stat_mods = e.stat_mods;
            break;
        case EffectKind::kDot:
        case EffectKind::kHot:
            // A DoT/HoT IS a periodic aura: its per-tick `amount` + `tick_ms` become
            // the periodic fields; damage for dot, heal for hot.
            aura.duration_ms = e.duration_ms;
            aura.periodic_kind =
                (e.kind == EffectKind::kDot) ? PeriodicKind::kDamage : PeriodicKind::kHeal;
            aura.periodic_amount_min = e.amount_min;
            aura.periodic_amount_max = e.amount_max;
            aura.periodic_tick_ms = e.tick_ms;
            break;
        case EffectKind::kBuff:
        case EffectKind::kDebuff:
            aura.duration_ms = e.duration_ms;
            aura.attr_ref = e.attribute;
            aura.attr_amount = e.attribute_amount;
            aura.attr_modifier = e.attribute_modifier;
            break;
        case EffectKind::kShield:
            aura.duration_ms = e.duration_ms;
            aura.shield_amount = roll_shield_amount(e, rng);
            host_.add_absorb(aura.shield_amount);  // grant the absorb pool
            break;
        case EffectKind::kCc:
            aura.duration_ms = e.duration_ms;
            aura.cc_kind = e.cc_kind;
            break;
        default:
            break;  // unreachable — is_container_kind gated above.
    }
    aura.remaining_ms = aura.duration_ms;

    fold_modifiers(aura, 1, +1);  // the first stack's modifier contribution
    auras_.push_back(std::move(aura));
    return {AuraApplyAction::kAdded, 1};
}

std::size_t AuraContainer::apply_ability_effects(const Ability& ability,
                                                 ObjectGuid caster_guid,
                                                 DispelType dispel_type, CombatRng* rng) {
    std::size_t applied = 0;
    for (std::uint32_t i = 0; i < ability.effects.size(); ++i) {
        if (is_container_kind(ability.effects[i].kind)) {
            const AuraApplyResult r = apply(ability, i, caster_guid, dispel_type, rng);
            if (r.action != AuraApplyAction::kRejected) ++applied;
        }
    }
    return applied;
}

// ---------------------------------------------------------------------------
// tick — periodic ticks + duration expiry
// ---------------------------------------------------------------------------

AuraTickResult AuraContainer::tick(std::uint32_t dt_ms, CombatRng& rng) {
    AuraTickResult result;
    if (dt_ms == 0) return result;

    for (ActiveAura& a : auras_) {
        // The aura is alive for at most its remaining duration within this step;
        // periodics only accrue over the time it is actually present.
        const std::uint32_t elapsed_alive =
            dt_ms < a.remaining_ms ? dt_ms : a.remaining_ms;

        if (a.is_periodic()) {
            a.since_last_tick_ms += elapsed_alive;
            while (a.since_last_tick_ms >= a.periodic_tick_ms) {
                a.since_last_tick_ms -= a.periodic_tick_ms;
                // A corpse takes no more DoT/HoT — stop firing once the host is
                // dead (do not consume further rolls). Duration still winds down.
                if (!host_.is_alive()) break;

                const std::uint32_t rolled =
                    rng.roll_amount(a.periodic_amount_min, a.periodic_amount_max);
                const std::uint32_t amount =
                    rolled * static_cast<std::uint32_t>(a.stacks);  // scale by stacks

                if (a.periodic_kind == PeriodicKind::kDamage) {
                    const DamageResult dr = host_.apply_damage(amount);
                    result.periodic_damage += dr.applied;
                    result.host_died = result.host_died || dr.lethal;
                } else {  // PeriodicKind::kHeal
                    result.periodic_healing += host_.apply_healing(amount);
                }
                ++result.ticks_fired;
            }
        }

        // Wind the duration down; mark for expiry when it reaches 0.
        a.remaining_ms = dt_ms < a.remaining_ms ? a.remaining_ms - dt_ms : 0;
    }

    // Sweep expired auras (remaining_ms == 0), rolling back their stat deltas.
    for (std::size_t i = auras_.size(); i-- > 0;) {
        if (auras_[i].remaining_ms == 0) {
            fold_modifiers(auras_[i], auras_[i].stacks, -1);
            auras_.erase(auras_.begin() + static_cast<std::ptrdiff_t>(i));
            ++result.auras_expired;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// remove / clear
// ---------------------------------------------------------------------------

bool AuraContainer::remove(AbilityId ability_id, std::uint32_t effect_index,
                           ObjectGuid caster_guid) {
    for (std::size_t i = 0; i < auras_.size(); ++i) {
        const ActiveAura& a = auras_[i];
        if (a.ability_id == ability_id && a.effect_index == effect_index &&
            a.caster_guid == caster_guid) {
            fold_modifiers(a, a.stacks, -1);
            auras_.erase(auras_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

std::size_t AuraContainer::dispel(DispelType type) {
    // kNone is the UNDISPELLABLE sentinel — dispelling it removes nothing, so
    // unclassified / undispellable auras always survive a dispel.
    if (type == DispelType::kNone) return 0;

    std::size_t removed = 0;
    // Back-to-front so erasures don't shift the yet-to-scan prefix. Every matching
    // instance is stripped — independent-caster copies and multi-stack instances
    // alike — rolling back the full (all-stacks) stat contribution of each.
    for (std::size_t i = auras_.size(); i-- > 0;) {
        if (auras_[i].dispel_type == type) {
            fold_modifiers(auras_[i], auras_[i].stacks, -1);
            auras_.erase(auras_.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        }
    }
    return removed;
}

void AuraContainer::clear() {
    // Reclaim every shield's absorb from the host before dropping the auras, so the
    // host's shield pool does not outlive the container that granted it (#693).
    for (const ActiveAura& a : auras_) {
        if (a.is_shield()) host_.remove_absorb(a.shield_amount);
    }
    for (std::size_t i = 0; i < kStatKeyCount; ++i) stat_totals_[i] = 0;
    attr_flat_.clear();
    attr_percent_.clear();
    auras_.clear();
}

}  // namespace meridian::worldd
