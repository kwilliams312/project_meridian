// Project Meridian — client kinematic movement controller CORE (issue #102).
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores"): this header
// and its .cpp contain NO Godot types. The GDExtension node
// (`MeridianMovementController`, meridian_movement_controller.*) is a thin
// wrapper that marshals Godot input/state in and out of this core. Keeping the
// simulation engine-free is the whole point of the #101 spike decision (query
// via a pure heightfield sample, NOT `CharacterBody3D`): the integrator must be
// bit-reproducible so reconciliation can re-simulate unacked inputs N times per
// frame (Client SAD §2.2 (b)) and so a plain-C++ doctest — and the server's
// movement track (#86) — can replay the SAME golden fixture (spike §4).
//
// This core implements the three things the spike (docs/movement-spike.md §3
// "What #102 implements") left to #102:
//   1. A fixed-20 Hz kinematic integrator using the LOCKED §2 constants
//      (movement_constants.h) and the `IWorldQuery` ground seam
//      (movement_query.h). Deterministic — no frame-coupled physics.
//   2. Prediction + reconciliation: a ring buffer of unacknowledged inputs;
//      on an authoritative MovementState (server seq) it discards acked inputs
//      and re-simulates the unacked ones FROM the server's authoritative
//      position, so a server correction / snap-back is reproduced exactly.
//   3. Movement-intent emission gated to the intent rate (≤ 10/s + on state
//      change — kMovementIntentMaxHz).
//
// The wire tables this feeds (schema/net/world.fbs) are MovementIntent (C→S,
// produced from `PredictedInput` + the predicted state) and MovementState (S→C,
// consumed by `reconcile`). The core deals in plain structs; FlatBuffers
// (de)serialization lives in the `net` module, off the main thread (SAD §6.1).

#ifndef MERIDIAN_MOVEMENT_CONTROLLER_H
#define MERIDIAN_MOVEMENT_CONTROLLER_H

#include "movement_constants.h"
#include "movement_query.h"

#include <cstdint>
#include <deque>
#include <optional>

namespace meridian::movement {

// ===========================================================================
// Plain value types (engine-free — no Godot Vector3).
// ===========================================================================

// A minimal 3D vector. y is UP (jump/gravity axis); x/z are the ground plane —
// consistent with `sample_ground(x, z) -> height (y)` in movement_query.h.
struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// Per-tick player input, sampled at the fixed sim rate (SAD §2.2 "fixed-tick
// input sampling"). Directions are in the character's local frame already
// resolved to world axes by the caller; magnitudes are unit (the integrator
// applies the locked speed for `mode`). `move_x` = strafe (+right), `move_z` =
// forward (+forward). `jump` requests a jump impulse if grounded this tick.
struct MovementInput {
	float    move_x      = 0.0f;   // [-1, 1] strafe axis (world X)
	float    move_z      = 0.0f;   // [-1, 1] forward axis (world Z)
	bool     jump        = false;  // edge-triggered jump request
	bool     walk        = false;  // walk toggle (else run) — picks the speed cap
	float    orientation = 0.0f;   // facing (radians) — carried onto the intent
};

// The full kinematic state the integrator advances and reconciliation rewinds
// to. Deterministic function of (prev state, input, dt, ground sample).
struct MovementSnapshot {
	Vec3     position;                          // zone-local metres
	Vec3     velocity;                          // m/s (y is vertical)
	bool     grounded    = true;                // resting on / clamped to ground
	MoveMode mode        = MoveMode::Idle;      // active locomotion mode this tick
	float    orientation = 0.0f;                // facing (radians)
};

// A record kept in the reconciliation ring buffer: the input applied at `seq`
// and the predicted state it PRODUCED. On a server ack we discard records with
// seq <= ack_seq; on a correction we re-simulate the survivors in order from the
// authoritative state (Client SAD §2.2 (b): "rewind to authoritative state,
// re-simulate unacked inputs").
struct PredictedInput {
	uint32_t         seq = 0;         // per-client monotonic (MovementIntent.seq)
	MovementInput    input;           // the input sampled at this tick
	MovementSnapshot predicted;       // state AFTER integrating `input`
};

// A MovementIntent to emit on the wire (schema/net/world.fbs MovementIntent).
// Plain struct — the `net` module encodes it to FlatBuffers off-thread.
struct MovementIntentOut {
	uint32_t seq            = 0;
	uint32_t state_flags    = 0;      // encoded from MovementInput (see intent_flags)
	float    x = 0.0f, y = 0.0f, z = 0.0f;
	float    orientation    = 0.0f;
	uint64_t client_time_ms = 0;
};

// An authoritative MovementState received from the server (schema MovementState),
// decoded by `net`. The fields the reconciler needs.
struct MovementStateIn {
	uint32_t ack_seq        = 0;      // last intent seq the server processed
	uint32_t state_flags    = 0;
	Vec3     position;                // authoritative position
	float    orientation    = 0.0f;
	uint64_t server_time_ms = 0;
};

// ===========================================================================
// state_flags wire bitfield (schema/net/world.fbs — "forward/back/strafe/jump/
// swim/sit/…"). M0 subset. The server (#86) decodes the LOW 3 BITS to recover the
// active MoveMode for its speed check, so client and server MUST agree on the
// encoding. The CANONICAL layout is defined ONCE in movement_constants.h §2b
// (kStateFlagsModeMask + mode_from_state_flags) and mirrored in the server header;
// this is the #247 fix that makes both sides agree without a wire-boundary hack.
//
//   bits 0..2 : MoveMode (kStateFlagsModeMask = 0x7) — the server's `& 0x7` read.
//   bit  3    : forward, bit 4 back, bit 5 strafeL, bit 6 strafeR (direction).
//   bit  7    : jump, bit 8 walk toggle. bits 9..31 reserved (swim/sit at M1).
//
// The direction/jump/walk flags therefore live ABOVE the 3 mode bits — they no
// longer collide with the mode selector the server reads (pre-#247 they occupied
// the low bits and a forward run decoded to the wrong mode).
// ===========================================================================
namespace flags {
inline constexpr uint32_t kForward = 1u << 3;
inline constexpr uint32_t kBack    = 1u << 4;
inline constexpr uint32_t kStrafeL = 1u << 5;
inline constexpr uint32_t kStrafeR = 1u << 6;
inline constexpr uint32_t kJump    = 1u << 7;
inline constexpr uint32_t kWalk    = 1u << 8;   // walk toggle active (else run)
} // namespace flags

// Encode a sampled input + resolved mode into the wire state_flags bitfield.
uint32_t encode_state_flags(const MovementInput& in, MoveMode mode);

// ===========================================================================
// The fixed-step integrator (pure function — the determinism guarantee).
// ===========================================================================
// Advance `prev` by ONE fixed tick (kTickSeconds) under `input`, resolving the
// ground via `world`. No time argument other than the locked dt: the sim step is
// constant so re-simulation reproduces integration exactly (spike §3 constraint
// 1). Pure w.r.t. its inputs — same (prev, input, world) always yields the same
// snapshot. This is the function BOTH prediction and reconciliation call, and
// the one the golden cross-track fixture pins (spike §4).
MovementSnapshot integrate_tick(const MovementSnapshot& prev,
                                const MovementInput& input,
                                const IWorldQuery& world);

// ===========================================================================
// Prediction + reconciliation (the ring buffer + rewind/re-sim).
// ===========================================================================
class PredictionReconciler {
public:
	// `world` must outlive the reconciler (the controller owns both). `start`
	// seeds the confirmed AND predicted state (e.g. spawn position from
	// EntityEnter).
	PredictionReconciler(const IWorldQuery& world, const MovementSnapshot& start);

	// Sample-and-predict one tick: assigns the next seq, integrates locally,
	// stores (seq, input, predicted) in the ring buffer, and returns the intent
	// to (maybe) emit. The returned intent is always well-formed; whether it is
	// ACTUALLY sent on the wire is gated by `should_emit_intent` (rate cap).
	MovementIntentOut predict(const MovementInput& input, uint64_t client_time_ms);

	// Apply an authoritative server state: discard acked inputs (seq <=
	// ack_seq), then rewind the predicted state to the server's authoritative
	// position and RE-SIMULATE every surviving unacked input in order. After
	// this, `predicted_state()` == server-authoritative + the local residual of
	// the still-unacked inputs (Client SAD §2.2 (b); the R5 cross-track
	// determinism check). Returns the post-reconciliation predicted state.
	MovementSnapshot reconcile(const MovementStateIn& server);

	// Rate-cap gate for intent emission (kMovementIntentMaxHz ≤ 10/s + on state
	// change). Returns true if an intent captured at `client_time_ms` with
	// `state_flags` should be SENT: either enough time has elapsed since the last
	// send (>= 1/kMovementIntentMaxHz), OR the movement state_flags changed since
	// the last send (edge — the "+ on state change" clause). Updates internal
	// last-send bookkeeping when it returns true.
	bool should_emit_intent(uint64_t client_time_ms, uint32_t state_flags);

	// The current predicted (client-visible) state.
	const MovementSnapshot& predicted_state() const { return predicted_; }

	// Number of unacknowledged inputs currently buffered (test/observability).
	std::size_t pending_count() const { return buffer_.size(); }

	// Last sequence number assigned by predict().
	uint32_t last_seq() const { return next_seq_ - 1; }

private:
	const IWorldQuery& world_;
	MovementSnapshot   predicted_;          // client-visible, N ticks ahead of ack
	std::deque<PredictedInput> buffer_;     // unacked inputs, ascending seq
	uint32_t next_seq_ = 1;                 // seq 0 reserved as "none"

	// Intent-rate bookkeeping.
	bool     have_sent_ = false;
	uint64_t last_sent_ms_ = 0;
	uint32_t last_sent_flags_ = 0;
};

} // namespace meridian::movement

#endif // MERIDIAN_MOVEMENT_CONTROLLER_H
