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

}  // namespace

// ---------------------------------------------------------------------------
// stat-mod folding + queries
// ---------------------------------------------------------------------------

void AuraContainer::fold_stat_mods(const ActiveAura& aura, std::uint16_t stack_count,
                                   int sign) {
    if (stack_count == 0) return;
    for (const StatMod& m : aura.stat_mods) {
        // amount is PER STACK: fold `stack_count` stacks' worth, with `sign`.
        stat_totals_[stat_index(m.stat)] +=
            sign * m.amount * static_cast<std::int32_t>(stack_count);
    }
}

std::int32_t AuraContainer::stat_delta(StatKey stat) const {
    return stat_totals_[stat_index(stat)];
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

AuraApplyResult AuraContainer::apply(const Ability& ability, std::uint32_t effect_index,
                                     ObjectGuid caster_guid, DispelType dispel_type) {
    // Guard: the effect must exist and be an aura. A non-aura effect (damage / heal
    // / threat) has no place in the container — reject without mutating anything.
    if (effect_index >= ability.effects.size()) {
        return {AuraApplyAction::kRejected, 0};
    }
    const AbilityEffect& e = ability.effects[effect_index];
    if (e.kind != EffectKind::kAura) {
        return {AuraApplyAction::kRejected, 0};
    }

    // Existing instance of THIS (ability, effect, caster)? Refresh or stack it.
    for (ActiveAura& a : auras_) {
        if (a.ability_id == ability.id && a.effect_index == effect_index &&
            a.caster_guid == caster_guid) {
            a.remaining_ms = a.duration_ms;  // refresh duration (both rules refresh)
            if (a.max_stacks > 1 && a.stacks < a.max_stacks) {
                // stack-count rule: gain exactly one stack, fold that stack's stats.
                a.stacks = static_cast<std::uint16_t>(a.stacks + 1);
                fold_stat_mods(a, 1, +1);
                return {AuraApplyAction::kStacked, a.stacks};
            }
            // refresh-duration rule (max_stacks == 1) OR already at the stack cap.
            return {AuraApplyAction::kRefreshed, a.stacks};
        }
    }

    // No existing instance — ADD a fresh one. A different caster reaches here even
    // for the same ability effect, giving an INDEPENDENT instance.
    ActiveAura aura;
    aura.ability_id = ability.id;
    aura.effect_index = effect_index;
    aura.caster_guid = caster_guid;
    aura.duration_ms = e.duration_ms;
    aura.max_stacks = e.max_stacks == 0 ? 1 : e.max_stacks;  // clamp a bad 0 to 1
    aura.periodic_kind = e.periodic_kind;
    aura.periodic_amount_min = e.periodic_amount_min;
    aura.periodic_amount_max = e.periodic_amount_max;
    aura.periodic_tick_ms = e.periodic_tick_ms;
    aura.stat_mods = e.stat_mods;
    aura.dispel_type = dispel_type;  // CMB-04 classification, fixed at first apply
    aura.stacks = 1;
    aura.remaining_ms = e.duration_ms;
    aura.since_last_tick_ms = 0;

    fold_stat_mods(aura, 1, +1);  // the first stack's stat contribution
    auras_.push_back(std::move(aura));
    return {AuraApplyAction::kAdded, 1};
}

std::size_t AuraContainer::apply_ability_auras(const Ability& ability,
                                               ObjectGuid caster_guid,
                                               DispelType dispel_type) {
    std::size_t applied = 0;
    for (std::uint32_t i = 0; i < ability.effects.size(); ++i) {
        if (ability.effects[i].kind == EffectKind::kAura) {
            const AuraApplyResult r = apply(ability, i, caster_guid, dispel_type);
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
            fold_stat_mods(auras_[i], auras_[i].stacks, -1);
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
            fold_stat_mods(a, a.stacks, -1);
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
            fold_stat_mods(auras_[i], auras_[i].stacks, -1);
            auras_.erase(auras_.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        }
    }
    return removed;
}

void AuraContainer::clear() {
    for (std::size_t i = 0; i < kStatKeyCount; ++i) stat_totals_[i] = 0;
    auras_.clear();
}

}  // namespace meridian::worldd
