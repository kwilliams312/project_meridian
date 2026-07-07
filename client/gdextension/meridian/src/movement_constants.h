// Project Meridian — shared movement constants (issue #101 spike).
//
// SINGLE SOURCE OF TRUTH for the movement constants that BOTH client-side
// prediction (CHR-02, Client SAD §2.2 / §3.3) AND server-side validation
// (OPS-03, Server SAD §5.5 / Server PRD §3.2) MUST agree on. "Server is law"
// (Baseline Pillar 3) requires the two simulations to share the same rules:
// the client predicts with these numbers and the server validates against the
// same numbers, so a well-behaved client never trips a correction.
//
// ─────────────────────────────────────────────────────────────────────────
//   ⚠ CROSS-BUILD-TREE CONTRACT — read before editing any value below.
// ─────────────────────────────────────────────────────────────────────────
// The client (this Godot GDExtension, C++) and the server (`worldd`, plain
// C++/Linux — Server SAD §1) are SEPARATE build trees that never link to each
// other (Client SAD §9.1/§9.2). There is therefore no compile-time shared
// header between them at M0. This file is the CLIENT's copy; the SERVER's
// movement validator (#86, Server SAD §5.5) MUST use byte-for-byte identical
// values. The single-source-of-truth mechanism is documented in
// docs/movement-spike.md §4 ("Keeping client and server in sync"):
//
//   • M0  — DOCUMENTED SYNC POINT. Both copies cite THIS spike (and the SAD
//           numbers it traces to). Any change is a two-file PR touching both
//           this header and the server validator, referencing #101. A golden
//           cross-track fixture (Client SAD R5: "golden fixtures shared with
//           server track") pins a set of (input, dt, expected-state) vectors
//           that BOTH simulations must reproduce — the CI trip-wire that makes
//           silent drift fail loudly.
//   • M1+ — the SAD's stated end-state (Client PRD §3.3, Client SAD §2.2):
//           "the movement constants live in shared content/schema data, not
//           duplicated magic numbers." `mcc` compiles one authoritative
//           constants record into BOTH the client `.pck` (IF-5) and the world
//           DB (IF-4). This header then becomes the loader's typed view /
//           compile-time default, not the authority. See §4 of the spike doc.
//
// TRACEABILITY: every value is tagged [LOCKED cite] (a hard SAD/PRD number) or
// [SPIKE-LOCKED] (this spike proposes it because no doc authored a number yet —
// the docs deliberately deferred concrete speeds to "shared content data").
// SPIKE-LOCKED values are the spike's decision-of-record for M0 and are the
// numbers #102 and #86 build against until `mcc` owns them at M1.

#ifndef MERIDIAN_MOVEMENT_CONSTANTS_H
#define MERIDIAN_MOVEMENT_CONSTANTS_H

#include <cstdint>

namespace meridian::movement {

// ===========================================================================
// 1. Simulation cadence — [LOCKED]
// ===========================================================================
// The server runs a fixed 20 Hz / 50 ms tick (Server SAD §3.2 "One 50 ms map
// tick"; Server PRD §3.1 "fixed 20 Hz (50 ms budget) world update"). The
// client's fixed prediction/simulation step (Client SAD §2.2 "fixed-tick input
// sampling", §6.1 "sim fixed-tick prediction") MUST match the server tick so
// re-simulation during reconciliation reproduces server integration exactly.
inline constexpr int      kServerTickHz      = 20;                    // [LOCKED: Server SAD §3.2 / Server PRD §3.1]
inline constexpr double   kTickSeconds       = 1.0 / kServerTickHz;   // 0.05 s — the fixed sim step dt
inline constexpr int      kTickMillis        = 50;                    // [LOCKED: Server SAD §3.2 — 50 ms budget]

// Client → server movement intent send rate: up to 10/s plus on state change
// (Server SAD §5.5 "≤ 10/s/client + state changes"; Server PRD §3.2). The sim
// ticks at 20 Hz but intents are coalesced (Client SAD §2.1 "coalescing for
// movement intents") down to this cap.
inline constexpr int      kMovementIntentMaxHz = 10;                  // [LOCKED: Server SAD §5.5 / Server PRD §3.2]

// ===========================================================================
// 2. Movement modes — M0 scope is walk / run / jump (CHR-02 basic)
// ===========================================================================
// Baseline CHR-02 "Movement & replication (walk/run/jump/swim) — M0 basic /
// M1". M0 basic = "Predicted walk/run/jump on the test map" (Client PRD §7 /
// Client SAD §7 M0 row). Swim, slope handling, fall damage, turn-in-place are
// M1 (Client PRD §7 M1: "Swim volumes … movement polish (slope handling)
// matched to server validation"). `state_flags` in MovementIntent/MovementState
// (schema/net/world.fbs) is the wire bitfield; MoveMode is the client's decoded
// active-locomotion mode used to pick the speed cap.
enum class MoveMode : uint8_t {
	Idle = 0,
	Walk = 1,
	Run  = 2,   // default forward locomotion in an MMO; toggles to Walk
	Jump = 3,   // airborne; horizontal speed carries from the pre-jump mode
	// Swim = 4,  // M1 (CHR-02 M1) — reserved, do not implement at M0
};

// ===========================================================================
// 3. Speeds (metres/second, zone-local) — [SPIKE-LOCKED]
// ===========================================================================
// No document authors concrete m/s speeds. Server SAD §5.5 and Server PRD §3.2
// both validate against "server-known speed for the active move mode" WITHOUT
// stating the number; Client PRD §3.3 says the constants "live in shared
// content/schema data, not duplicated magic numbers." So THIS SPIKE locks the
// M0 numbers. Values are the WoW-lineage reference the game design leans on
// (CMaNGOS/TrinityCore are the acknowledged architectural references — Baseline
// §2, Server SAD clean-room note): WoW run ≈ 7 yd/s ≈ 6.4 m/s; walk ≈ 2.5 m/s;
// backpedal ≈ 4.5 yd/s ≈ 2.5 m/s. Rounded to clean SI values for M0.
// These become `mcc`-owned content data at M1 (§4 of the spike doc); until then
// they are the decision-of-record shared by #102 (client) and #86 (server).
inline constexpr float    kRunSpeed          = 6.0f;   // [SPIKE-LOCKED] forward run, m/s
inline constexpr float    kWalkSpeed         = 2.5f;   // [SPIKE-LOCKED] walk toggle, m/s
inline constexpr float    kBackpedalSpeed    = 2.5f;   // [SPIKE-LOCKED] backward, m/s (WoW-style penalty)
inline constexpr float    kStrafeSpeed       = 6.0f;   // [SPIKE-LOCKED] lateral == run at M0 (no strafe penalty yet)

// Returns the authoritative max ground speed for a mode — the SAME function the
// server's speed check evaluates (Server SAD §5.5: displacement vs
// `server_speed(active_mode) × Δt × tol`). Client and server MUST return
// identical results; that is the whole point of sharing this header.
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
// THE ONE authoritative definition of how MovementIntent/MovementState.state_flags
// (schema/net/world.fbs, a uint32) is packed. Both the CLIENT encoder (#102
// encode_state_flags) and the SERVER decoder (#86 mode_from_flags) key off THIS
// layout; a well-behaved client encodes it and the server decodes the SAME bits to
// the SAME MoveMode, so no wire-boundary fix-up is needed (#247 — the encoding
// mismatch this replaces; previously the bot #111 stamped the mode at the wire
// boundary).
//
//   ┌──────────────── uint32 state_flags ────────────────┐
//   │ bits 31..6 reserved (0) │ 5 walk │ 4 jump │ 3 strafeR │ 2 strafeL │ 1 back │ 0 fwd │  ← NO
//   └─────────────────────────────────────────────────────┘
// NO — that was the pre-#247 client layout (direction in the low bits) which the
// server misread as a mode. CANONICAL layout instead:
//
//   bits 0..2  (mask kModeMask = 0x7) : MoveMode enum (Idle=0/Walk=1/Run=2/Jump=3).
//                                       The server reads `state_flags & 0x7` here to
//                                       pick server_speed(mode). THIS is the field
//                                       the #247 fix makes both sides agree on.
//   bit  3     (kFwd)     : forward     (move_z > 0)
//   bit  4     (kBack)    : backward    (move_z < 0)
//   bit  5     (kStrafeL) : strafe left (move_x < 0)
//   bit  6     (kStrafeR) : strafe right(move_x > 0)
//   bit  7     (kJump)    : jump requested this tick
//   bit  8     (kWalk)    : walk toggle active (redundant with mode==Walk, kept for
//                           the richer S→C broadcast layout / future observers)
//   bits 9..31 : reserved (0) — swim/sit/… land here at M1 (CHR-02).
//
// The low 3 bits are the mode selector, matching the server's `& 0x7` read
// (movement_validation.cpp mode_from_flags) and the MoveMode enum values above.
inline constexpr std::uint32_t kStateFlagsModeMask = 0x7u;   // low 3 bits = MoveMode

// Extract the MoveMode the low 3 bits encode. Unknown values (only 4..7 are
// possible in 3 bits, and 4=Swim is M1-reserved) fall back to Run — the SAFEST
// default: it grants the LARGEST M0 ground-speed budget, so an unrecognised value
// never spuriously rejects a legal move (the cap only tightens for Walk/Idle).
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
// Not authored anywhere. For M0 the spike locks INSTANT acceleration to top
// speed (accel effectively ∞): the reconciliation math (Client SAD §2.2/§3.3)
// and the server speed check (§5.5, per-packet + 2 s sliding window) are both
// simplest and most robust when speed is a step function of input, and WoW-
// lineage ground movement is effectively instant-accel. A finite accel/decel
// ramp is an M1 "movement polish" item (Client PRD §7 M1) if feel demands it;
// it MUST land in both trees together if adopted. kGroundAccel documents the
// intent; #102 may treat 0 as the "instant" sentinel.
inline constexpr float    kGroundAccel       = 0.0f;   // [SPIKE-LOCKED] 0 == instant (step to target speed)

// ===========================================================================
// 5. Gravity & jump — [SPIKE-LOCKED], within M0 CHR-02 (jump is in scope)
// ===========================================================================
// Jump IS in M0 scope ("predicted walk/run/jump", Client PRD §7 / SAD §7 M0).
// No document authors gravity or jump-impulse numbers. The spike locks a
// real-world-plausible gravity and a jump impulse giving a ~1.0 m apex (a
// readable MMO hop, tuned for feel at M1). The server's bounds check tolerates
// the jump arc because it validates z only to ±4 m of the heightfield (§6),
// far larger than any jump apex — so gravity/jump are prediction-feel numbers
// the server does not tightly police at M0, but they still MUST match so the
// re-simulated arc lands where the server expects on touchdown.
inline constexpr float    kGravity           = 20.0f;  // [SPIKE-LOCKED] m/s² downward (snappier than 9.81 — MMO feel)
inline constexpr float    kJumpSpeed         = 6.3f;   // [SPIKE-LOCKED] initial +y m/s; apex ≈ v²/2g ≈ 0.99 m
inline constexpr float    kTerminalFallSpeed = 60.0f;  // [SPIKE-LOCKED] clamp fall speed (safety; well inside ±4 m/tick)

// ===========================================================================
// 6. Server validation tolerances — [LOCKED] (client mirrors so it can
//    self-check / predict corrections)
// ===========================================================================
// Server SAD §5.5, verbatim contract the server validator enforces and the
// client predicts against:
//   • speed:  displacement ≤ server_speed(mode) × Δt × 1.15  per packet,
//             AND over a sliding 2 s window (catches burst-then-idle cheats).
//   • bounds: z within ±4 m of the heightfield/navmesh sample (plausibility,
//             not collision honesty — full collision is client-side).
inline constexpr float    kSpeedTolerance    = 1.15f;  // [LOCKED: Server SAD §5.5 — "× 1.15 per packet"]
inline constexpr double   kSpeedWindowSeconds = 2.0;   // [LOCKED: Server SAD §5.5 — "sliding 2 s window"]
inline constexpr float    kHeightTolerance   = 4.0f;   // [LOCKED: Server SAD §5.5 — "z within ±4 m of heightfield sample"]

// Per-packet max plausible ground displacement the server accepts, given the
// intent cap: run × (1/intentHz) × tol. Client can use this to know when its
// own prediction would be corrected. (Derived, not independently authored.)
inline constexpr float    kMaxPacketDisplacement =
	kRunSpeed * (1.0f / kMovementIntentMaxHz) * kSpeedTolerance;  // ≈ 0.69 m

// ===========================================================================
// 7. World query geometry — [LOCKED] (see docs/movement-spike.md §3 decision)
// ===========================================================================
// The heightfield the server validates against is `ITerrainBackend::
// export_heightfield(chunk) → f32[129×129]` (Tools SAD §5.2, §3.3; Sync
// Decisions §11): a 129×129 row-major float32 grid at 1 m spacing spanning a
// 128 m chunk (128 m + 1 shared-edge sample), in zone-local metres. The client
// kinematic controller queries the SAME heightfield data (shipped in the client
// `.pck` per chunk, IF-6) so its ground sample matches the server's — this is
// the "query method" decision (spike §3): heightfield bilinear sample, NOT a
// Godot PhysicsServer raycast or CharacterBody3D move_and_slide.
inline constexpr int      kHeightfieldSide   = 129;    // [LOCKED: Tools SAD §3.3 / §5.2 — 129×129]
inline constexpr float    kHeightfieldSpacingM = 1.0f; // [LOCKED: Tools SAD §3.3 — 1 m spacing]
inline constexpr float    kChunkSizeM        = 128.0f; // [LOCKED: IF-6 / D-20 — 128 m chunk]

// NOTE (D-19): M0 itself runs on a FLAT bootstrap test map with BOUNDS-ONLY
// validation — no heightfield/navmesh consumption (Sync Decisions §5, D-19).
// So the heightfield query path is DESIGNED here and stubbed (§3 spike + the
// stub in movement_query.h) but only wired to real terrain at M1 with Zone-01
// greybox. At M0 the ground sample is a constant plane (y = 0).

} // namespace meridian::movement

#endif // MERIDIAN_MOVEMENT_CONSTANTS_H
