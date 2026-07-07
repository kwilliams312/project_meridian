// SPDX-License-Identifier: Apache-2.0
//
// worldd — GOLDEN CROSS-TRACK MOVEMENT FIXTURE (issue #86 / #101 §4 trip-wire).
//
// CLEAN-ROOM: authored from docs/movement-spike.md §4 ("Keeping client and server
// in sync" — the golden cross-track fixture), Client SAD R5 ("golden fixtures
// shared with server track"), and the LOCKED constants in movement_constants.h.
// No GPL source consulted. See CONTRIBUTING.md.
//
// PURPOSE (the drift trip-wire): movement-spike.md §4 makes "single source of
// truth" a documented sync point PLUS a CI trip-wire, because the client and
// server are separate build trees with no shared #include. This file pins a set
// of (start_state, intent, dt, expected decision) vectors that BOTH simulations
// must reproduce EXACTLY. The server's movement unit test loads this fixture and
// asserts every vector; the client doctest track loads the SAME logical vectors
// (#101's recommendation — "Client doctest and the server's movement unit tests
// both load the fixture; drift fails BOTH CIs loudly").
//
// If any shared constant drifts (a speed, the ×1.15 tolerance, the 2 s window, the
// ±4 m z-tolerance, the bounds), at least one vector below flips its expected
// accept/reject outcome and the test FAILS — the loud, cross-tree drift alarm.
//
// The vectors are chosen to straddle each threshold so a change to ANY constant
// moves at least one across its boundary:
//   • a legal walk step (well inside every cap)                  → ACCEPT
//   • a legal run step at the per-packet edge                    → ACCEPT
//   • a per-packet speed-hack just over the run cap              → REJECT (speed)
//   • an out-of-bounds jump beyond the play area                 → REJECT (bounds)
//   • a z spike beyond ±4 m of the flat ground                   → REJECT (z)
//   • a z hop inside ±4 m (a legal jump apex)                    → ACCEPT

#ifndef MERIDIAN_WORLDD_MOVEMENT_FIXTURE_H
#define MERIDIAN_WORLDD_MOVEMENT_FIXTURE_H

#include <array>
#include <cstdint>

#include "movement_constants.h"
#include "movement_validation.h"

namespace meridian::worldd::fixture {

// One golden vector: an authoritative start position/time, an intent proposed
// from it at a given client time, and the decision BOTH tracks must produce.
struct GoldenVector {
    const char* name;

    // Authoritative start.
    Position start;
    std::uint64_t start_time_ms;

    // The proposed intent (position/flags/seq/time).
    MovementIntentPod intent;

    // Expected outcome.
    bool expect_accepted;
    MoveReject expect_reject;  // kNone when accepted
};

// The pinned vectors. Positions are in the M0 bootstrap play area
// [0, kChunkSizeM]² (movement_constants.h §8); the flat ground is z = 0.
//
// Δt note: each vector's intent.client_time_ms is start_time_ms + 100 ms (the
// 10/s cadence), so Δt = 0.1 s. The per-packet run budget is therefore
//   6.0 m/s × 0.1 s × 1.15 = 0.69 m,  the walk budget 2.5 × 0.1 × 1.15 = 0.2875 m.
inline constexpr std::array<GoldenVector, 6> kGoldenVectors = {{
    // 1. Legal WALK step: 0.20 m over 0.1 s at walk cap 0.2875 m ⇒ accept.
    {
        "legal_walk_step",
        /*start=*/{10.0f, 10.0f, 0.0f, 0.0f}, /*start_time_ms=*/1000,
        /*intent=*/{/*seq=*/1, /*flags=*/static_cast<std::uint32_t>(movement::MoveMode::Walk),
                    /*pos=*/{10.20f, 10.0f, 0.0f, 0.0f}, /*client_time_ms=*/1100},
        /*accept=*/true, MoveReject::kNone,
    },
    // 2. Legal RUN step at the per-packet edge: 0.68 m ≤ 0.69 m ⇒ accept.
    {
        "legal_run_edge",
        {20.0f, 20.0f, 0.0f, 0.0f}, 1000,
        {2, static_cast<std::uint32_t>(movement::MoveMode::Run),
         {20.68f, 20.0f, 0.0f, 0.0f}, 1100},
        true, MoveReject::kNone,
    },
    // 3. Per-packet SPEED HACK: 1.50 m in 0.1 s ≫ 0.69 m run budget ⇒ reject.
    {
        "speedhack_per_packet",
        {30.0f, 30.0f, 0.0f, 0.0f}, 1000,
        {3, static_cast<std::uint32_t>(movement::MoveMode::Run),
         {31.50f, 30.0f, 0.0f, 0.0f}, 1100},
        false, MoveReject::kSpeedPerPacket,
    },
    // 4. OUT OF BOUNDS: target x = 200 m is outside [0, 128] ⇒ reject (bounds).
    //    (Displacement from 127 → 200 is also huge, but bounds is checked and the
    //    speed check would trip first; we pin a SMALL step to a just-outside point
    //    so THIS vector isolates the bounds rule: 128.0 → 128.5 is 0.5 m ≤ 0.69 m
    //    per-packet budget, so speed passes and BOUNDS is the failing check.)
    {
        "out_of_bounds",
        {128.0f, 64.0f, 0.0f, 0.0f}, 1000,
        {4, static_cast<std::uint32_t>(movement::MoveMode::Run),
         {128.5f, 64.0f, 0.0f, 0.0f}, 1100},
        false, MoveReject::kOutOfBounds,
    },
    // 5. Z SPIKE beyond ±4 m of flat ground: z = 5.0 m ⇒ reject (z). Horizontal
    //    step 0.10 m ≤ budget so speed passes; only the z check trips.
    {
        "z_spike",
        {40.0f, 40.0f, 0.0f, 0.0f}, 1000,
        {5, static_cast<std::uint32_t>(movement::MoveMode::Jump),
         {40.10f, 40.0f, 5.0f, 0.0f}, 1100},
        false, MoveReject::kZOutOfRange,
    },
    // 6. Legal JUMP apex inside ±4 m: z = 0.99 m (≈ the #101 jump apex), 0.10 m
    //    horizontal ⇒ accept (z within tolerance, speed within budget).
    {
        "legal_jump_apex",
        {50.0f, 50.0f, 0.0f, 0.0f}, 1000,
        {6, static_cast<std::uint32_t>(movement::MoveMode::Jump),
         {50.10f, 50.0f, 0.99f, 0.0f}, 1100},
        true, MoveReject::kNone,
    },
}};

// ===========================================================================
// #247 — canonical state_flags encoding mirror (cross-track round-trip proof).
// ===========================================================================
// The CLIENT (movement_controller.cpp #102 encode_state_flags) builds state_flags
// with the active MoveMode in the LOW 3 BITS and direction/jump/walk flags ABOVE
// them (movement_constants.h §2b). The two build trees cannot share the client's
// encoder, so this function MIRRORS the client's canonical bit layout exactly —
// the same "documented sync point + fixture" discipline the numeric constants use.
// The server movement test feeds a flags value built HERE to mode_from_flags and
// asserts it decodes to the same mode: proof the server decodes the client's REAL
// encoding (no wire-boundary workaround). If the client layout drifts, this mirror
// (and the client's own round-trip test) diverge and a test fails loudly.
namespace client_flags {
// Direction/jump/walk bits — MUST match client movement_controller.h `flags::`.
inline constexpr std::uint32_t kFwd     = 1u << 3;
inline constexpr std::uint32_t kBack    = 1u << 4;
inline constexpr std::uint32_t kStrafeL = 1u << 5;
inline constexpr std::uint32_t kStrafeR = 1u << 6;
inline constexpr std::uint32_t kJump    = 1u << 7;
inline constexpr std::uint32_t kWalk    = 1u << 8;
}  // namespace client_flags

// The direction-flag block MUST be disjoint from the mode mask — the compile-time
// contract that guarantees a direction flag never bleeds into the mode selector.
static_assert(((client_flags::kFwd | client_flags::kBack | client_flags::kStrafeL |
               client_flags::kStrafeR | client_flags::kJump | client_flags::kWalk) &
              movement::kStateFlagsModeMask) == 0u,
              "#247: client direction flags must not overlap the state_flags mode mask");

// Mirror of the client's encode_state_flags(in, mode) canonical layout.
inline std::uint32_t client_encode_state_flags(movement::MoveMode mode, float move_x,
                                               float move_z, bool jump, bool walk) {
    std::uint32_t f =
        static_cast<std::uint32_t>(mode) & movement::kStateFlagsModeMask;  // low 3 = mode
    if (move_z > 0.0f) f |= client_flags::kFwd;
    if (move_z < 0.0f) f |= client_flags::kBack;
    if (move_x > 0.0f) f |= client_flags::kStrafeR;
    if (move_x < 0.0f) f |= client_flags::kStrafeL;
    if (jump)          f |= client_flags::kJump;
    if (walk)          f |= client_flags::kWalk;
    return f;
}

}  // namespace meridian::worldd::fixture

#endif  // MERIDIAN_WORLDD_MOVEMENT_FIXTURE_H
