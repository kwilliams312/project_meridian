// Project Meridian — client kinematic movement controller CORE (issue #102).
// Engine-free implementation (no Godot) — see movement_controller.h header note.
//
// TRACE: docs/movement-spike.md §3 "What #102 implements"; Client SAD §2.2
// (prediction/reconciliation, fixed-tick sampling), §6.1 (fixed-tick sim).
// All physics numbers come from movement_constants.h (the #101 LOCKED header) —
// this file never hard-codes a speed/gravity/dt.

#include "movement_controller.h"

#include <cmath>

namespace meridian::movement {

// ---------------------------------------------------------------------------
// state_flags encoding (shared wire contract with server #86).
// ---------------------------------------------------------------------------
// CANONICAL layout (movement_constants.h §2b, #247): the LOW 3 BITS carry the
// active MoveMode (the field the server reads with `state_flags & 0x7` to pick the
// speed cap); the direction/jump/walk flags sit ABOVE them. The client stamps the
// mode here so the server decodes the correct cap directly — no wire-boundary
// fix-up (the pre-#247 bot #111 workaround) is needed.
uint32_t encode_state_flags(const MovementInput& in, MoveMode mode) {
	// Low 3 bits: the active MoveMode (server reads state_flags & kStateFlagsModeMask).
	uint32_t f = static_cast<uint32_t>(mode) & kStateFlagsModeMask;
	// Direction / jump / walk flags live above the mode bits.
	if (in.move_z > 0.0f) f |= flags::kForward;
	if (in.move_z < 0.0f) f |= flags::kBack;
	if (in.move_x > 0.0f) f |= flags::kStrafeR;
	if (in.move_x < 0.0f) f |= flags::kStrafeL;
	if (in.jump)          f |= flags::kJump;
	if (in.walk)          f |= flags::kWalk;
	return f;
}

// ---------------------------------------------------------------------------
// Locomotion-mode resolution from input (picks the speed cap).
// ---------------------------------------------------------------------------
// Instant-accel step function (kGroundAccel == 0, spike §4): the mode is a pure
// function of the current input, so horizontal speed steps to server_speed(mode)
// immediately. Backpedal gets its own (slower) cap per WoW-lineage.
static MoveMode resolve_mode(const MovementInput& in, bool grounded) {
	const bool moving = (in.move_x != 0.0f) || (in.move_z != 0.0f);
	if (!grounded) return MoveMode::Jump;   // airborne: horizontal cap = run (constants §5)
	if (!moving)   return MoveMode::Idle;
	return in.walk ? MoveMode::Walk : MoveMode::Run;
}

// Horizontal speed cap for this input/mode. Backpedal (pure backward, no strafe)
// uses kBackpedalSpeed; everything else uses server_speed(mode) so client and
// server agree (server validates displacement vs server_speed()).
static float horizontal_speed(const MovementInput& in, MoveMode mode) {
	if (mode == MoveMode::Walk) return kWalkSpeed;
	// Backpedal penalty: moving straight back on the ground.
	if (mode == MoveMode::Run && in.move_z < 0.0f && in.move_x == 0.0f) {
		return kBackpedalSpeed;
	}
	return server_speed(mode);   // Run / Jump(airborne) -> kRunSpeed; Idle -> 0
}

// ---------------------------------------------------------------------------
// The fixed-step integrator — ONE deterministic tick.
// ---------------------------------------------------------------------------
MovementSnapshot integrate_tick(const MovementSnapshot& prev,
                                const MovementInput& input,
                                const IWorldQuery& world) {
	const float dt = static_cast<float>(kTickSeconds);   // 0.05 s, locked
	MovementSnapshot s = prev;
	s.orientation = input.orientation;

	// --- 1. Horizontal velocity: instant step to the mode's speed cap. -------
	// Normalize the ground input direction so diagonal movement is not faster.
	const MoveMode mode = resolve_mode(input, prev.grounded);
	s.mode = mode;

	float dx = input.move_x;
	float dz = input.move_z;
	const float len = std::sqrt(dx * dx + dz * dz);
	if (len > 0.0f) {
		dx /= len;
		dz /= len;
	}
	const float speed = horizontal_speed(input, mode);
	s.velocity.x = dx * speed;
	s.velocity.z = dz * speed;

	// --- 2. Vertical velocity: jump impulse + gravity integration. -----------
	if (prev.grounded && input.jump) {
		s.velocity.y = kJumpSpeed;   // launch
		s.grounded = false;
	} else if (!prev.grounded) {
		s.velocity.y = prev.velocity.y - kGravity * dt;   // fall/rise under gravity
		if (s.velocity.y < -kTerminalFallSpeed) {
			s.velocity.y = -kTerminalFallSpeed;
		}
	} else {
		s.velocity.y = 0.0f;   // grounded, not jumping
	}

	// --- 3. Integrate position (semi-implicit Euler with the new velocity). ---
	s.position.x = prev.position.x + s.velocity.x * dt;
	s.position.z = prev.position.z + s.velocity.z * dt;
	s.position.y = prev.position.y + s.velocity.y * dt;

	// --- 4. Resolve against the ground via the query seam (spike §3). --------
	// FlatWorldQuery at M0 (y=0 plane); HeightfieldWorldQuery at M1 — drop-in.
	const GroundSample g = world.sample_ground(s.position.x, s.position.z);
	if (!g.walkable) {
		// The ground under this XZ is UNKNOWN — the covering chunk has not streamed
		// in yet (HeightfieldWorldQuery returns {0, false} for a non-resident cell)
		// or it is a hole. NEVER clamp the character down onto the guessed y=0 plane:
		// that is the "fall through the world before the chunk arrives" bug this story
		// (#558) must make impossible. Hold the current y (server-authoritative until
		// the chunk is resident) and neutralise fall velocity so no downward drift
		// accumulates. Once the chunk streams in this branch stops firing and the
		// normal clamp/airborne logic below resumes. (FlatWorldQuery is always
		// walkable, so the M0 path is unchanged.)
		s.velocity.y = 0.0f;
		s.grounded   = true;
	} else if (s.position.y <= g.height) {
		// Landed / on ground: clamp to the surface, kill downward velocity.
		s.position.y = g.height;
		if (s.velocity.y <= 0.0f) {
			s.velocity.y = 0.0f;
			s.grounded = true;
		}
	} else {
		// Above the ground -> airborne (walked off an edge, or mid-jump).
		s.grounded = false;
	}

	return s;
}

// ---------------------------------------------------------------------------
// PredictionReconciler
// ---------------------------------------------------------------------------
PredictionReconciler::PredictionReconciler(const IWorldQuery& world,
                                           const MovementSnapshot& start)
    : world_(world), predicted_(start) {}

MovementIntentOut PredictionReconciler::predict(const MovementInput& input,
                                                uint64_t client_time_ms) {
	// Integrate one tick locally from the current predicted state.
	const MovementSnapshot next = integrate_tick(predicted_, input, world_);
	predicted_ = next;

	const uint32_t seq = next_seq_++;
	buffer_.push_back(PredictedInput{seq, input, next});

	MovementIntentOut out;
	out.seq            = seq;
	out.state_flags    = encode_state_flags(input, next.mode);
	out.x              = next.position.x;
	out.y              = next.position.y;
	out.z              = next.position.z;
	out.orientation    = next.orientation;
	out.client_time_ms = client_time_ms;
	return out;
}

// Time-parameterised decay curve for the render error offset (#103). Maps elapsed
// time in [0, window] to a scale in [1, 0]. Smoothstep falloff (1 - (3u²-2u³)):
// monotonically DECREASING on [0,1] (its derivative -6u(1-u) ≤ 0), C1-continuous at
// both ends for a soft start/stop, and EXACTLY zero at the window end — so the
// offset converges (the #103 monotonic-decay/convergence property) independent of
// frame rate. dt-agnostic: we scale the captured start offset by f(elapsed), we do
// not multiply the running offset frame-by-frame (which would be frame-rate coupled).
static float decay_scale(uint64_t elapsed_ms) {
	const uint64_t window = reconcile_tuning::kErrorDecayWindowMs;
	if (elapsed_ms >= window) return 0.0f;
	const float u = static_cast<float>(elapsed_ms) / static_cast<float>(window);
	return 1.0f - (3.0f * u * u - 2.0f * u * u * u);
}

MovementSnapshot PredictionReconciler::reconcile(const MovementStateIn& server) {
	// Capture where the player is currently being SHOWN (sim + any in-flight decay
	// offset) BEFORE adopting the correction, so the visible position stays
	// continuous across the reconcile — a mid-decay correction composes cleanly
	// with the one still resolving instead of popping.
	const Vec3 visible_before{
	    predicted_.position.x + error_offset_.x,
	    predicted_.position.y + error_offset_.y,
	    predicted_.position.z + error_offset_.z,
	};

	// 1. Discard acknowledged inputs: the server has processed everything with
	//    seq <= ack_seq, so those predictions are now confirmed and irrelevant.
	while (!buffer_.empty() && buffer_.front().seq <= server.ack_seq) {
		buffer_.pop_front();
	}

	// 2. Rewind to the authoritative state. This is the server's snap-back
	//    point — whatever it corrected us to, we accept as ground truth.
	MovementSnapshot state = predicted_;
	state.position    = server.position;
	state.orientation = server.orientation;
	// Re-derive grounded/velocity plausibly from the authoritative position:
	// the server sends position only, so we resolve grounded against the same
	// ground sample and let the re-simulation below rebuild velocity from input.
	const GroundSample g = world_.sample_ground(state.position.x, state.position.z);
	// Over ground that is not yet resident (walkable == false) the height is unknown,
	// so treat the authoritative position as supported rather than deriving "airborne"
	// and integrating a fall on the next tick (#558 no-fall-through). FlatWorldQuery is
	// always walkable, so the M0 reconcile path is unchanged.
	state.grounded = (!g.walkable) || (state.position.y <= g.height + 1e-4f);
	state.velocity = Vec3{};

	// 3. Re-simulate every UNACKED input in order, from the authoritative state.
	//    Because integrate_tick is deterministic and uses the same locked
	//    constants + ground query, re-running the surviving inputs reproduces
	//    the client's local residual on top of the server's correction — a
	//    smooth reconciliation, not a hard reset to the (stale) server position.
	//    Also refresh each buffered record's stored prediction so a subsequent
	//    reconcile rewinds/re-sims from a consistent chain.
	for (PredictedInput& rec : buffer_) {
		state = integrate_tick(state, rec.input, world_);
		rec.predicted = state;
	}

	predicted_ = state;

	// 4. Error-offset smoothing (#103). The error is how far the corrected sim sits
	//    from where we were SHOWING the player (visible_before). Small error ->
	//    capture it as a render offset so the visible position does not pop, then
	//    decay it to zero over the window in advance_smoothing(). Large error ->
	//    snap (drop the offset; the visible position jumps to the corrected sim).
	const Vec3 err{
	    visible_before.x - predicted_.position.x,
	    visible_before.y - predicted_.position.y,
	    visible_before.z - predicted_.position.z,
	};
	last_error_mag_ = std::sqrt(err.x * err.x + err.y * err.y + err.z * err.z);

	if (last_error_mag_ > reconcile_tuning::kSnapThresholdMeters) {
		// Snap: too large to hide — show the corrected sim immediately.
		error_offset_       = Vec3{};
		error_offset_start_ = Vec3{};
		smoothing_elapsed_ms_ = 0;
		smoothing_active_   = false;
		last_snapped_       = true;
	} else if (last_error_mag_ > 0.0f) {
		// Small error: begin (or restart) the decay from the full offset.
		error_offset_       = err;
		error_offset_start_ = err;
		smoothing_elapsed_ms_ = 0;
		smoothing_active_   = true;
		last_snapped_       = false;
	} else {
		// Exact match: nothing to smooth; clear any residual offset.
		error_offset_       = Vec3{};
		error_offset_start_ = Vec3{};
		smoothing_elapsed_ms_ = 0;
		smoothing_active_   = false;
		last_snapped_       = false;
	}

	return predicted_;
}

void PredictionReconciler::advance_smoothing(uint64_t dt_ms) {
	if (!smoothing_active_) return;
	smoothing_elapsed_ms_ += dt_ms;
	if (smoothing_elapsed_ms_ >= reconcile_tuning::kErrorDecayWindowMs) {
		// Window elapsed: offset is exactly zero, visible == corrected sim.
		smoothing_elapsed_ms_ = reconcile_tuning::kErrorDecayWindowMs;
		error_offset_     = Vec3{};
		smoothing_active_ = false;
		return;
	}
	const float f = decay_scale(smoothing_elapsed_ms_);
	error_offset_.x = error_offset_start_.x * f;
	error_offset_.y = error_offset_start_.y * f;
	error_offset_.z = error_offset_start_.z * f;
}

MovementSnapshot PredictionReconciler::visible_state() const {
	MovementSnapshot s = predicted_;
	s.position.x += error_offset_.x;
	s.position.y += error_offset_.y;
	s.position.z += error_offset_.z;
	return s;
}

bool PredictionReconciler::should_emit_intent(uint64_t client_time_ms,
                                              uint32_t state_flags) {
	// Always emit the very first intent.
	if (!have_sent_) {
		have_sent_       = true;
		last_sent_ms_    = client_time_ms;
		last_sent_flags_ = state_flags;
		return true;
	}

	// "+ on state change": a movement-flag edge sends immediately (bounded below
	// by the sim rate, not the intent cap — a real input change is important).
	if (state_flags != last_sent_flags_) {
		last_sent_ms_    = client_time_ms;
		last_sent_flags_ = state_flags;
		return true;
	}

	// Steady state: cap at kMovementIntentMaxHz (≤ 10/s). Minimum interval in ms.
	constexpr uint64_t kMinIntervalMs =
	    static_cast<uint64_t>(1000 / kMovementIntentMaxHz);   // 100 ms
	if (client_time_ms - last_sent_ms_ >= kMinIntervalMs) {
		last_sent_ms_    = client_time_ms;
		last_sent_flags_ = state_flags;
		return true;
	}

	return false;
}

} // namespace meridian::movement
