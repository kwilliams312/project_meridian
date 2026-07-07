// SPDX-License-Identifier: Apache-2.0
//
// worldd — SERVER-SIDE movement constants (issue #86, server half of IT-M0).
//
// CLEAN-ROOM: authored from docs/movement-spike.md (#101) + docs/sad/server-sad.md
// §5.5 (movement validation envelope) and §3.2 (the 50 ms / 20 Hz tick) only. No
// GPL source (CMaNGOS / TrinityCore or otherwise) consulted. See CONTRIBUTING.md.
//
// ─────────────────────────────────────────────────────────────────────────
//   ⚠ CROSS-BUILD-TREE CONTRACT — this is the SERVER's copy of the #101 constants.
// ─────────────────────────────────────────────────────────────────────────
// "Server is law" (Baseline Pillar 3): the server validator (OPS-03, SAD §5.5)
// MUST validate against values that are BYTE-FOR-BYTE IDENTICAL to the client's
// prediction constants, so a well-behaved client never trips a correction. The
// client (Godot GDExtension) and the server (worldd, plain C++/Linux — SAD §1)
// are SEPARATE build trees that never link (movement-spike.md §1), so there is no
// shared #include at M0. The single-source-of-truth mechanism (movement-spike.md
// §4) is a DOCUMENTED SYNC POINT + a CI TRIP-WIRE, not a shared header:
//
//   • The client's authoritative copy is
//     client/gdextension/meridian/src/movement_constants.h (#101).
//   • THIS header is the server's copy. Every value below is transcribed from
//     that client header and MUST match it exactly. Each value cites both #101
//     (the spike that locked it) and the SAD number it traces to.
//   • Any change to a shared movement number is a SINGLE PR touching BOTH the
//     client header AND this server header, referencing #101.
//   • The GOLDEN CROSS-TRACK FIXTURE (movement-spike.md §4, Client SAD R5) is the
//     trip-wire: a set of (start_state, intent, dt, expected_state) vectors that
//     BOTH simulations must reproduce. It is defined in movement_fixture.h and
//     exercised by the server's movement unit test (test/movement_validation_test.cpp)
//     AND, per #101's recommendation, mirrored on the client doctest track. Drift
//     in any shared constant changes a fixture output → the test fails loudly on
//     BOTH CIs. The static_assert block at the bottom of THIS file is the
//     compile-time first line of defence: it pins each transcribed value so an
//     accidental edit here fails the server build immediately, before the fixture
//     even runs.
//   • M1+: the SAD end-state (Client PRD §3.3) is that mcc compiles ONE
//     authoritative constants record into both trees; this header then becomes a
//     loader view, no longer the authority. The [SPIKE-LOCKED] speeds migrate
//     first.
//
// TRACEABILITY tags mirror the client header: [LOCKED cite] = a hard SAD/PRD
// number; [SPIKE-LOCKED] = #101 locked it because no doc authored a number.

#ifndef MERIDIAN_WORLDD_MOVEMENT_CONSTANTS_H
#define MERIDIAN_WORLDD_MOVEMENT_CONSTANTS_H

#include <cstdint>

namespace meridian::worldd::movement {

// ===========================================================================
// 1. Simulation cadence — [LOCKED]
// ===========================================================================
// Fixed 20 Hz / 50 ms map tick (Server SAD §3.2 "One 50 ms map tick"; Server PRD
// §3.1 "fixed 20 Hz (50 ms budget)"). The client's fixed prediction step matches
// it (movement-spike.md §2). dt is the per-tick integration step the validator
// measures displacement against when it advances the authoritative position.
inline constexpr int      kServerTickHz        = 20;                  // [LOCKED: SAD §3.2 / PRD §3.1] (mirrors client kServerTickHz)
inline constexpr double   kTickSeconds         = 1.0 / kServerTickHz; // 0.05 s (mirrors client kTickSeconds)
inline constexpr int      kTickMillis          = 50;                  // [LOCKED: SAD §3.2 — 50 ms] (mirrors client kTickMillis)

// Client → server movement-intent send rate: ≤ 10/s + on state change (SAD §5.5
// "≤ 10/s/client + state changes"; PRD §3.2). The server rate-limits intake to
// this class (BUILD step 1). Intents above the cap that are NOT a state change
// are dropped/coalesced.
inline constexpr int      kMovementIntentMaxHz = 10;                  // [LOCKED: SAD §5.5 / PRD §3.2] (mirrors client kMovementIntentMaxHz)

// ===========================================================================
// 2. Movement modes — M0 scope is walk / run / jump (CHR-02 basic)
// ===========================================================================
// Mirrors the client's MoveMode (movement_constants.h §2). state_flags in
// MovementIntent/MovementState (schema/net/world.fbs) is the wire bitfield; the
// server decodes it to a MoveMode to pick the authoritative speed cap. Swim is
// reserved (M1, CHR-02) — not implemented; a Swim flag at M0 is treated as Run.
enum class MoveMode : std::uint8_t {
    Idle = 0,
    Walk = 1,
    Run  = 2,   // default forward locomotion
    Jump = 3,   // airborne; horizontal cap carries from the pre-jump mode
    // Swim = 4,  // M1 (CHR-02 M1) — reserved, not implemented at M0
};

// ===========================================================================
// 3. Speeds (metres/second, zone-local) — [SPIKE-LOCKED]
// ===========================================================================
// No doc authors concrete m/s speeds; #101 locked the M0 numbers (WoW-lineage
// reference, rounded to clean SI). These MUST equal the client header's values.
inline constexpr float    kRunSpeed            = 6.0f;   // [SPIKE-LOCKED #101] forward run (mirrors client kRunSpeed)
inline constexpr float    kWalkSpeed           = 2.5f;   // [SPIKE-LOCKED #101] walk toggle (mirrors client kWalkSpeed)
inline constexpr float    kBackpedalSpeed      = 2.5f;   // [SPIKE-LOCKED #101] backward (mirrors client kBackpedalSpeed)
inline constexpr float    kStrafeSpeed         = 6.0f;   // [SPIKE-LOCKED #101] lateral == run at M0 (mirrors client kStrafeSpeed)

// The authoritative max ground speed for a mode — the SAME mapping the client's
// server_speed(mode) returns (movement_constants.h §3). SAD §5.5 validates
// displacement against server_speed(active_mode) × Δt × tolerance.
constexpr float server_speed(MoveMode mode) {
    switch (mode) {
        case MoveMode::Walk: return kWalkSpeed;
        case MoveMode::Run:  return kRunSpeed;
        case MoveMode::Jump: return kRunSpeed;  // horizontal cap while airborne
        case MoveMode::Idle: return 0.0f;
        default:             return kRunSpeed;
    }
}

// ===========================================================================
// 2b. state_flags WIRE BIT LAYOUT — CANONICAL, shared client (#102) + server (#86)
// ===========================================================================
// The SERVER's copy of the canonical MovementIntent/MovementState.state_flags
// layout (schema/net/world.fbs, uint32). This MUST match the client's copy in
// client/gdextension/meridian/src/movement_constants.h §2b byte-for-byte — same
// documented-sync-point + static_assert discipline as the numeric constants above
// (#247 canonical encoding). CANONICAL layout:
//
//   bits 0..2  (mask kStateFlagsModeMask = 0x7) : MoveMode (Idle=0/Walk=1/Run=2/
//                                       Jump=3). mode_from_flags() reads THIS field.
//   bit  3  fwd | bit 4 back | bit 5 strafeL | bit 6 strafeR : direction
//   bit  7  jump | bit 8 walk-toggle : the remaining M0 movement flags
//   bits 9..31 : reserved (0) — swim/sit/… at M1.
//
// The low 3 bits are the mode selector — exactly the `state_flags & 0x7` read
// mode_from_flags() performs. Before #247 the client put DIRECTION bits in the low
// 3 (so a forward run decoded to Walk), and the bot #111 stamped the mode in at the
// wire boundary to compensate; the client now encodes THIS canonical layout so the
// server decodes the right mode directly.
inline constexpr std::uint32_t kStateFlagsModeMask = 0x7u;   // low 3 bits = MoveMode

// Extract the MoveMode the low 3 bits encode — the shared canonical decode the
// validator's mode_from_flags() delegates to (must match the client header §2b
// mode_from_state_flags byte-for-byte). Unknown values (only 4..7 fit in 3 bits;
// 4=Swim is M1-reserved) fall back to Run — the SAFEST default (largest M0
// ground-speed budget, so an unrecognised value never spuriously rejects a legal
// move; the cap only tightens for Walk/Idle).
constexpr MoveMode mode_from_state_flags(std::uint32_t state_flags) {
    switch (state_flags & kStateFlagsModeMask) {
        case static_cast<std::uint32_t>(MoveMode::Idle): return MoveMode::Idle;
        case static_cast<std::uint32_t>(MoveMode::Walk): return MoveMode::Walk;
        case static_cast<std::uint32_t>(MoveMode::Run):  return MoveMode::Run;
        case static_cast<std::uint32_t>(MoveMode::Jump): return MoveMode::Jump;
        default:                                         return MoveMode::Run;
    }
}

// ===========================================================================
// 4. Acceleration — [SPIKE-LOCKED]
// ===========================================================================
// #101 locks INSTANT acceleration (0 == step to target speed). Keeps the §5.5
// speed check a simple displacement-vs-cap comparison (no ramp integration).
inline constexpr float    kGroundAccel         = 0.0f;   // [SPIKE-LOCKED #101] 0 == instant (mirrors client kGroundAccel)

// ===========================================================================
// 5. Gravity & jump — [SPIKE-LOCKED], within M0 CHR-02 (jump in scope)
// ===========================================================================
// The server does NOT tightly police the jump arc at M0 (z is validated only to
// ±4 m of the flat ground, far larger than the ≈0.99 m apex — §6), but the
// numbers are carried here so the shared-constant set is complete and the golden
// fixture can exercise a jump vector identically on both tracks.
inline constexpr float    kGravity             = 20.0f;  // [SPIKE-LOCKED #101] m/s² down (mirrors client kGravity)
inline constexpr float    kJumpSpeed           = 6.3f;   // [SPIKE-LOCKED #101] initial +y m/s (mirrors client kJumpSpeed)
inline constexpr float    kTerminalFallSpeed   = 60.0f;  // [SPIKE-LOCKED #101] fall clamp (mirrors client kTerminalFallSpeed)

// ===========================================================================
// 6. Server validation tolerances — [LOCKED] (SAD §5.5)
// ===========================================================================
// The verbatim §5.5 contract this validator enforces:
//   • speed:  displacement ≤ server_speed(mode) × Δt × 1.15  per packet, AND
//             over a sliding 2 s window (catches burst-then-idle cheats).
//   • bounds: z within ±4 m of the heightfield sample (flat world = 0 at M0, D-19).
inline constexpr float    kSpeedTolerance      = 1.15f;  // [LOCKED: SAD §5.5 "× 1.15"] (mirrors client kSpeedTolerance)
inline constexpr double   kSpeedWindowSeconds  = 2.0;    // [LOCKED: SAD §5.5 "sliding 2 s window"] (mirrors client kSpeedWindowSeconds)
inline constexpr float    kHeightTolerance     = 4.0f;   // [LOCKED: SAD §5.5 "z within ±4 m of heightfield"] (mirrors client kHeightTolerance)

// Per-packet max plausible ground displacement given the intent cap:
//   run × (1/intentHz) × tol  ≈ 0.69 m. (Derived — mirrors client
// kMaxPacketDisplacement.) A single-packet displacement above this is corrected.
inline constexpr float    kMaxPacketDisplacement =
    kRunSpeed * (1.0f / kMovementIntentMaxHz) * kSpeedTolerance;  // ≈ 0.69 m

// ===========================================================================
// 7. World query geometry — [LOCKED] + M0 flat-world ground (D-19)
// ===========================================================================
// The heightfield the server WILL validate z against at M1 is f32[129×129] @ 1 m
// over a 128 m chunk (Tools SAD §5.2/§3.3). At M0 (D-19) worldd runs on a FLAT
// bootstrap map with BOUNDS-ONLY validation — the ground sample is a constant
// plane y = 0. The z check is |proposed_z − kFlatGroundZ| ≤ kHeightTolerance.
inline constexpr int      kHeightfieldSide     = 129;    // [LOCKED: Tools SAD §3.3/§5.2] (mirrors client kHeightfieldSide)
inline constexpr float    kHeightfieldSpacingM = 1.0f;   // [LOCKED: Tools SAD §3.3] (mirrors client kHeightfieldSpacingM)
inline constexpr float    kChunkSizeM          = 128.0f; // [LOCKED: IF-6 / D-20] (mirrors client kChunkSizeM)

// M0 flat-world ground height (D-19: heightfield sample is a constant plane).
// The server samples this instead of a real heightfield until M1/Zone-01.
inline constexpr float    kFlatGroundZ         = 0.0f;   // [D-19] flat bootstrap map ground plane

// ===========================================================================
// 8. Zone bounds (M0 bootstrap map) — [D-19 / SAD §5.5 "inside map bounds"]
// ===========================================================================
// SAD §5.5 requires the move be "inside map bounds". D-19's flat bootstrap map
// has no authored extent yet, so M0 uses the heightfield chunk footprint as the
// zone-local play area: the [0, kChunkSizeM] × [0, kChunkSizeM] square (the same
// 128 m chunk the heightfield geometry describes). Positions outside this square
// are a hard bounds violation → snap-back. This is a conservative M0 stand-in;
// the real per-zone bounds arrive with Zone-01 at M1 (D-19).
inline constexpr float    kZoneMinXY           = 0.0f;                 // [D-19 M0] bootstrap play-area min (x and y)
inline constexpr float    kZoneMaxXY           = kChunkSizeM;          // [D-19 M0] bootstrap play-area max (x and y) = 128 m

// ===========================================================================
// Compile-time drift trip-wire (movement-spike.md §4 first line of defence).
// ===========================================================================
// These static_asserts pin every SHARED value transcribed from the client header
// to its literal. If someone edits a value above without going through the §4
// two-file sync PR, the server build fails HERE — before the golden fixture even
// runs. The literals are duplicated ON PURPOSE: this block only passes if the
// declared constant still equals the number #101 locked. (Cross-tree equality
// with the client is additionally proven by the shared golden fixture, since the
// two build trees cannot share a static_assert.)
static_assert(kServerTickHz == 20,                 "cadence drift vs #101 (kServerTickHz)");
static_assert(kTickMillis == 50,                   "cadence drift vs #101 (kTickMillis)");
static_assert(kMovementIntentMaxHz == 10,          "intent-rate drift vs #101 (kMovementIntentMaxHz)");
static_assert(kRunSpeed == 6.0f,                   "speed drift vs #101 (kRunSpeed)");
static_assert(kWalkSpeed == 2.5f,                  "speed drift vs #101 (kWalkSpeed)");
static_assert(kBackpedalSpeed == 2.5f,             "speed drift vs #101 (kBackpedalSpeed)");
static_assert(kStrafeSpeed == 6.0f,                "speed drift vs #101 (kStrafeSpeed)");
static_assert(kGroundAccel == 0.0f,                "accel drift vs #101 (kGroundAccel)");
static_assert(kGravity == 20.0f,                   "gravity drift vs #101 (kGravity)");
static_assert(kJumpSpeed == 6.3f,                  "jump drift vs #101 (kJumpSpeed)");
static_assert(kTerminalFallSpeed == 60.0f,         "terminal-fall drift vs #101 (kTerminalFallSpeed)");
static_assert(kSpeedTolerance == 1.15f,            "tolerance drift vs #101 (kSpeedTolerance)");
static_assert(kSpeedWindowSeconds == 2.0,          "window drift vs #101 (kSpeedWindowSeconds)");
static_assert(kHeightTolerance == 4.0f,            "z-tolerance drift vs #101 (kHeightTolerance)");
static_assert(kHeightfieldSide == 129,             "heightfield-side drift vs #101 (kHeightfieldSide)");
static_assert(kHeightfieldSpacingM == 1.0f,        "heightfield-spacing drift vs #101 (kHeightfieldSpacingM)");
static_assert(kChunkSizeM == 128.0f,               "chunk-size drift vs #101 (kChunkSizeM)");
static_assert(server_speed(MoveMode::Run) == kRunSpeed,    "server_speed(Run) must return kRunSpeed");
static_assert(server_speed(MoveMode::Walk) == kWalkSpeed,  "server_speed(Walk) must return kWalkSpeed");
static_assert(server_speed(MoveMode::Jump) == kRunSpeed,   "server_speed(Jump) must cap at run speed");
static_assert(server_speed(MoveMode::Idle) == 0.0f,        "server_speed(Idle) must be 0");

// #247 canonical state_flags layout: the low 3 bits ARE the MoveMode selector, and
// the mode mask must be exactly 0x7. If the client shifts the mode field or widens
// the mask without a matching two-file PR, the round-trip fixture (test) flips —
// this pins the mode-bit contract at compile time (mirrors the client header §2b).
static_assert(kStateFlagsModeMask == 0x7u,                 "state_flags mode mask drift vs #247 (must be low 3 bits)");
static_assert(static_cast<std::uint32_t>(MoveMode::Idle) == 0u, "MoveMode::Idle must occupy state_flags value 0");
static_assert(static_cast<std::uint32_t>(MoveMode::Walk) == 1u, "MoveMode::Walk must occupy state_flags value 1");
static_assert(static_cast<std::uint32_t>(MoveMode::Run)  == 2u, "MoveMode::Run must occupy state_flags value 2");
static_assert(static_cast<std::uint32_t>(MoveMode::Jump) == 3u, "MoveMode::Jump must occupy state_flags value 3");

}  // namespace meridian::worldd::movement

#endif  // MERIDIAN_WORLDD_MOVEMENT_CONSTANTS_H
