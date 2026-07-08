// SPDX-License-Identifier: Apache-2.0
//
// worldd — movement-derived (fall + swim/breath) damage implementation (issue #362,
// CHR-02 full). See movement_damage.h for the design + clean-room provenance
// (docs/prd/server-prd.md §4-M1 + docs/sad/server-sad.md §2.5/§3.2 only).

#include "movement_damage.h"

#include <algorithm>
#include <cmath>

namespace meridian::worldd {

namespace {

// Round a fraction-of-max-health to an absolute HP amount, deterministically.
// `frac` is clamped to [0, 1] first so a curve can never remove more than the pool.
std::uint32_t scale_health(float frac, std::uint32_t max_health) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    const long hp = std::lround(static_cast<double>(frac) * static_cast<double>(max_health));
    if (hp <= 0) return 0;
    return static_cast<std::uint32_t>(hp);
}

}  // namespace

bool MovementDamageState::compute_airborne(const Position& pos, const MovementEnv& env) const {
    return pos.z > env.ground_z + params_.fall_airborne_epsilon_m;
}

bool MovementDamageState::compute_submerged(const Position& pos, const MovementEnv& env) const {
    if (!env.water_surface_z.has_value()) return false;  // no water here ⇒ never submerged
    return pos.z < (*env.water_surface_z - params_.submerge_depth_m);
}

MovementDamageResult MovementDamageState::step(const Position& pos, const MovementEnv& env,
                                               std::uint32_t dt_ms, std::uint32_t max_health) {
    MovementDamageResult r;

    const bool now_airborne = compute_airborne(pos, env);
    const bool submerged = compute_submerged(pos, env);
    r.submerged = submerged;

    // --- FIRST STEP: seed the tracker; no landing can be inferred yet. ----------
    if (!initialized_) {
        initialized_ = true;
        airborne_ = now_airborne;
        apex_z_ = pos.z;
        breath_ms_ = params_.breath_capacity_ms;
    }

    // --- SWIM / BREATH ----------------------------------------------------------
    // Submerged: drain breath 1 ms per elapsed ms; once empty, accrue drown time and
    // fire a drown tick every interval. Surfaced: refill (faster than it drained)
    // and clear any partial drown accrual (a breath of air stops the drowning).
    if (submerged) {
        // The slice of THIS step spent underwater with no breath left (0 while any
        // breath remains) — only that time drives drowning, so the step that drains
        // the last of the breath is not also charged as a full drowning interval.
        const std::uint32_t drown_ms = dt_ms > breath_ms_ ? dt_ms - breath_ms_ : 0;
        breath_ms_ = breath_ms_ > dt_ms ? breath_ms_ - dt_ms : 0;
        if (drown_ms > 0) {
            drown_accum_ms_ += drown_ms;
            while (params_.drown_tick_interval_ms > 0 &&
                   drown_accum_ms_ >= params_.drown_tick_interval_ms) {
                drown_accum_ms_ -= params_.drown_tick_interval_ms;
                ++r.drown_ticks;
            }
            if (r.drown_ticks > 0) {
                r.drowning = true;
                // Per-tick HP: the fraction of max health, floored to at least 1 so a
                // low-HP unit still drowns rather than taking 0 forever.
                std::uint32_t per = scale_health(params_.drown_tick_frac, max_health);
                if (per == 0 && max_health > 0) per = 1;
                r.drown_damage = per * r.drown_ticks;
            }
        }
    } else {
        const std::uint64_t refill =
            static_cast<std::uint64_t>(dt_ms) * params_.breath_refill_rate;
        const std::uint64_t topped = static_cast<std::uint64_t>(breath_ms_) + refill;
        breath_ms_ = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(topped, params_.breath_capacity_ms));
        drown_accum_ms_ = 0;  // surfaced — the drowning clock resets
    }
    r.breath_remaining_ms = breath_ms_;

    // --- FALL -------------------------------------------------------------------
    // Track the apex while airborne; on the airborne→grounded transition, the drop
    // is apex-above-landing. Damage only above the safe band, and never when the
    // landing is INTO water (water breaks the fall — the swim seam feeds `submerged`).
    if (now_airborne) {
        if (!airborne_) {
            apex_z_ = pos.z;  // just left the ground — start a fresh apex
        } else {
            apex_z_ = std::max(apex_z_, pos.z);
        }
    } else {
        if (airborne_) {  // LANDED this step
            r.landed = true;
            const float fall_height = apex_z_ - pos.z;
            r.fall_height_m = fall_height;
            if (!submerged && fall_height > params_.fall_safe_height_m) {
                const float frac =
                    params_.fall_damage_frac_per_m * (fall_height - params_.fall_safe_height_m);
                r.fall_damage = scale_health(frac, max_health);
            }
        }
        apex_z_ = pos.z;  // grounded — the apex tracks the ground until the next takeoff
    }
    airborne_ = now_airborne;

    return r;
}

}  // namespace meridian::worldd
