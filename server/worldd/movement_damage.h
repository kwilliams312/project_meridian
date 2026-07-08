// SPDX-License-Identifier: Apache-2.0
//
// worldd — movement-derived (fall + swim/breath) damage (issue #362, CHR-02 full;
// part of epic #19). The server-authoritative half of the CHR-02 envelope that the
// M0 basic movement (#86 intake/validation + AoI relay) deliberately deferred to
// M1 (Server PRD §4-M1 "CHR-02 (full): swim, fall damage").
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md §3.2 / §4-M1 ("CHR-02 (full):
// swim, fall damage") + §5.5 lineage (the movement-validation state worldd already
// derives), docs/sad/server-sad.md §2.5/§3.2 (the single-threaded per-map tick that
// owns entity state + advances it 20 Hz), and the #342 Unit model + #86 movement
// state / #101 movement_constants ONLY. No GPL / AGPL / CMaNGOS / TrinityCore /
// leaked emulator source consulted — every threshold, curve, and cadence here is
// ORIGINAL, derived from OUR docs (see the numbers' rationale inline + CONTRIBUTING).
//
// WHAT THIS FILE IS: a PURE, per-unit evaluator that turns a stream of authoritative
// positions (the ones #86 validated and worldd advances each tick) into two kinds of
// environmental damage the MapTick combat phase (#349) then applies via
// Unit::apply_damage:
//
//   • FALL damage — when a unit LANDS (returns to the ground after being airborne),
//     damage scaled by how far it fell ABOVE a safe threshold. A drop within the
//     safe band (an ordinary jump: ≈0.99 m apex at #101's kJumpSpeed/kGravity) does
//     NO damage; beyond it, damage rises linearly with the excess height as a
//     fraction of the unit's max health, reaching lethal at a hard fall. Landing IN
//     water breaks the fall (no damage) — the swim seam below feeds that in.
//
//   • SWIM / BREATH — while a unit is SUBMERGED (head below the water surface) its
//     breath depletes; once breath is exhausted, DROWNING damage ticks on a fixed
//     cadence (a fraction of max health per tick); SURFACING refills breath (faster
//     than it drains) and stops the drowning.
//
// PURITY (so it is DB-free / socket-free / clock-free UNIT-TESTABLE, like the rest of
// the tick modules): the evaluator reads NO wall clock (dt is passed in), touches NO
// socket / DB / FlatBuffer, and rolls NO RNG — fall/breath are deterministic
// functions of (position stream, dt, environment, max_health). A given input stream
// yields the same damage on every platform, so the whole module runs in the plain
// server ctest with no MariaDB (mirrors map_tick / aoi_grid / movement_validation).
//
// THREADING (SAD §2.5/§6): a map is single-threaded — "the tick owns entity state".
// A MovementDamageState is owned by the one MapTick that drives it (one per player);
// it carries NO lock. It never reaches into a Unit — the caller passes max_health in
// and applies the returned amount, so this module has zero dependency on combat_unit.

#ifndef MERIDIAN_WORLDD_MOVEMENT_DAMAGE_H
#define MERIDIAN_WORLDD_MOVEMENT_DAMAGE_H

#include <cstdint>
#include <optional>

#include "movement_constants.h"    // kFlatGroundZ (M0 flat-world ground plane)
#include "movement_validation.h"   // Position

namespace meridian::worldd {

// ---------------------------------------------------------------------------
// The environment a unit is evaluated against at its current (x, y). Supplied by
// the caller each step so the SAME pure evaluator serves the M0 flat bootstrap map
// (ground = kFlatGroundZ, no water) and, unchanged, an M1 zone with a heightfield +
// water volumes (the caller swaps in the sampled ground / water-surface heights).
// ---------------------------------------------------------------------------
struct MovementEnv {
    // The ground height under the unit's (x, y). At M0 this is the flat plane
    // kFlatGroundZ (D-19); at M1 it is the heightfield sample the #86 validator's
    // ground seam already resolves. A unit is "airborne" when its z is above this.
    float ground_z = movement::kFlatGroundZ;

    // The water surface height at the unit's (x, y), or nullopt where there is no
    // water (the whole M0 flat map). A unit is "submerged" when its z is below the
    // surface (minus the params' submerge depth). nullopt ⇒ never submerged.
    std::optional<float> water_surface_z;
};

// ---------------------------------------------------------------------------
// Tunable thresholds / curves / cadences. ORIGINAL clean-room numbers (see the
// per-field rationale). Defaults are the production values; a test constructs a
// state with tightened numbers so a fast deterministic scenario exercises every
// branch without simulating minutes of ticks.
// ---------------------------------------------------------------------------
struct MovementDamageParams {
    // --- FALL --------------------------------------------------------------
    // How far above the ground a unit's z must be to count as airborne (so float
    // noise / a unit resting exactly on the ground never registers a fall). Small.
    float fall_airborne_epsilon_m = 0.05f;

    // The SAFE fall height: a landing after falling this far or less does NO damage.
    // Chosen well above #101's jump apex (≈0.99 m at kJumpSpeed 6.3 / kGravity 20)
    // so an ordinary jump is always harmless, with margin for stepping off low
    // ledges. Original — no doc authors a number; derived from the jump kinematics.
    float fall_safe_height_m = 5.0f;

    // Damage per metre fallen ABOVE the safe height, as a FRACTION of max health.
    // 0.10/m means: a landing 5 m past the safe band (10 m total) costs 50% max HP,
    // and 10 m past it (15 m total) is lethal (100%). Linear + clamped to [0,1] —
    // a clean, testable curve derived originally (not from any game's fall table).
    float fall_damage_frac_per_m = 0.10f;

    // --- SWIM / BREATH -----------------------------------------------------
    // How far a unit's z must be BELOW the water surface to count as submerged
    // (head underwater). 0 = submerged as soon as the reference point dips below the
    // surface — the simple M0 model (a full water-line/eyeline model is later work).
    float submerge_depth_m = 0.0f;

    // Lungful of air: how long a unit can stay submerged before drowning begins.
    // 45 s is an original, playable breath window (long enough to cross a pond,
    // short enough that a deep dive is a real risk). Depletes 1 ms per elapsed ms.
    std::uint32_t breath_capacity_ms = 45'000;

    // Once breath is exhausted, drowning damage ticks every this-many ms …
    std::uint32_t drown_tick_interval_ms = 1'000;
    // … removing this FRACTION of max health per tick (min 1 HP so a low-HP unit
    // still drowns). 0.10/tick ⇒ ~10 s from empty lungs to death — deliberate,
    // recoverable-if-you-surface pacing. Original clean-room number.
    float drown_tick_frac = 0.10f;

    // Surfacing refills breath this many times faster than it drains (so a gasp of
    // air recovers quicker than the dive spent it). 3× ⇒ a full 45 s window refills
    // in ~15 s of surface time. Original.
    std::uint32_t breath_refill_rate = 3;
};

// ---------------------------------------------------------------------------
// The outcome of ONE step. `*_damage` are absolute HP the caller removes via
// Unit::apply_damage; the flags/heights let a test (and the tick's event stream)
// assert what happened without re-deriving it.
// ---------------------------------------------------------------------------
struct MovementDamageResult {
    // FALL: set on the step the unit lands (airborne→grounded). fall_height_m is the
    // apex-above-landing drop; fall_damage is 0 within the safe band OR when the
    // landing is into water (water breaks the fall).
    bool          landed = false;
    float         fall_height_m = 0.0f;
    std::uint32_t fall_damage = 0;

    // SWIM/BREATH: submerged says the unit is underwater this step; breath_remaining_ms
    // is what is left of the lungful; drowning fires once breath hits 0; drown_ticks
    // is how many drown intervals elapsed this step and drown_damage their total HP.
    bool          submerged = false;
    std::uint32_t breath_remaining_ms = 0;
    bool          drowning = false;
    std::uint32_t drown_ticks = 0;
    std::uint32_t drown_damage = 0;

    // The total HP to remove this step (fall + drown). The tick applies this via
    // Unit::apply_damage in one call.
    std::uint32_t total_damage() const { return fall_damage + drown_damage; }
};

// ---------------------------------------------------------------------------
// MovementDamageState — per-unit fall/breath tracker. Feed it the unit's
// authoritative position each tick via step(); it returns the damage that tick.
// ---------------------------------------------------------------------------
class MovementDamageState {
public:
    explicit MovementDamageState(const MovementDamageParams& params = {})
        : params_(params) {}

    // Advance one step for a unit now at `pos`, over `dt_ms` elapsed, in `env`, with
    // `max_health` (for scaling fall/drown fractions to absolute HP). The FIRST call
    // only seeds the tracker (no landing can be inferred without a prior state);
    // subsequent calls detect landings + accrue breath/drowning. Deterministic.
    MovementDamageResult step(const Position& pos, const MovementEnv& env,
                              std::uint32_t dt_ms, std::uint32_t max_health);

    // Test / diagnostic accessors.
    std::uint32_t breath_ms() const { return breath_ms_; }
    bool airborne() const { return airborne_; }
    const MovementDamageParams& params() const { return params_; }

private:
    // Is `pos.z` above `env.ground_z` by more than the airborne epsilon?
    bool compute_airborne(const Position& pos, const MovementEnv& env) const;
    // Is `pos.z` below the water surface (minus submerge depth)? False if no water.
    bool compute_submerged(const Position& pos, const MovementEnv& env) const;

    MovementDamageParams params_;

    bool  initialized_ = false;
    bool  airborne_ = false;
    float apex_z_ = 0.0f;  // highest z reached in the current airborne episode

    std::uint32_t breath_ms_ = 0;         // remaining breath (seeded to capacity)
    std::uint32_t drown_accum_ms_ = 0;    // time past empty-lungs not yet ticked
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_MOVEMENT_DAMAGE_H
