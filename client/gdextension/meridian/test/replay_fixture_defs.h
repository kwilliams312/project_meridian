// Project Meridian — shared golden-fixture RECORDINGS for the replay harness (#106).
//
// ONE definition of each checked-in replay recording, included by BOTH the fixture
// generator (replay_fixture_gen.cpp — writes test/fixtures/*.replay) and the test
// (replay_harness_test.cpp — loads those fixtures and asserts a fresh replay is
// bit-identical to the checked-in golden). Because both build the recording from
// THIS header, the test proves two things at once: (a) the on-disk fixture parses
// back to the same recording, and (b) replaying that recording still reproduces the
// golden the generator captured. If the sim ever drifts numerically, the checked-in
// golden stops matching and the test fails — the desync regression net.
//
// Engine-free: pure movement structs, no Godot.

#ifndef MERIDIAN_REPLAY_FIXTURE_DEFS_H
#define MERIDIAN_REPLAY_FIXTURE_DEFS_H

#include "movement_constants.h"
#include "replay_harness.h"

#include <cstdint>
#include <string>
#include <vector>

namespace meridian::replay::fixtures {

// A forward-run input on flat ground (unit forward, run mode).
inline MovementInput fwd_run() {
	MovementInput in;
	in.move_z = 1.0f;   // forward
	in.walk   = false;  // run
	return in;
}

// ---------------------------------------------------------------------------
// Fixture 1: "forward_run_small_correction"
// ---------------------------------------------------------------------------
// The bread-and-butter case: 24 forward-run ticks with two SMALL lateral server
// corrections (sub-snap-threshold) that engage the #103 error-offset decay, each
// followed by render-time advance_smoothing steps. Exercises predict + rewind/
// re-simulate + monotonic decay — the everyday prediction/reconciliation path a
// well-behaved client walks every second.
inline Recording forward_run_small_correction() {
	Recording rec;
	rec.world_plane_y = 0.0f;   // M0 flat bootstrap map (D-19)
	rec.start = MovementSnapshot{};   // spawn at origin, grounded, idle

	const float dt   = static_cast<float>(movement::kTickSeconds);
	const float step = movement::kRunSpeed * dt;   // per-tick forward advance

	for (int i = 0; i < 24; ++i) {
		ReplayEvent ev;
		ev.client_time_ms = static_cast<uint64_t>(i) * 50;   // 20 Hz
		ev.input = fwd_run();

		// A small lateral nudge at tick 8 and tick 16: the server acks the first
		// few ticks and reports a slightly different x (sub-threshold -> smoothed).
		if (i == 8 || i == 16) {
			ev.has_correction         = true;
			ev.correction.ack_seq     = static_cast<uint32_t>(i - 3);
			ev.correction.position.x  = (i == 8) ? 0.15f : -0.10f;   // < 1.0 m snap thr
			ev.correction.position.y  = 0.0f;
			ev.correction.position.z  = step * static_cast<float>(i - 3);
			ev.correction.server_time_ms = ev.client_time_ms;
			ev.advance_ms             = 30;   // begin decaying the captured offset
		} else if (i == 9 || i == 10 || i == 17 || i == 18) {
			// Continue the render-time decay on the ticks after each correction.
			ev.advance_ms = 40;
		}
		rec.events.push_back(ev);
	}
	return rec;
}

// ---------------------------------------------------------------------------
// Fixture 2: "jump_arc_snap_correction"
// ---------------------------------------------------------------------------
// The airborne + hard-correction case: a jump (grounded -> airborne gravity arc,
// exercising integrate_tick's vertical path + ground clamp on touchdown), then a
// LARGE server correction that exceeds the snap threshold (a genuine desync / a
// teleport) — the reconciler SNAPS. Proves the harness captures the snap path and
// the full jump arc deterministically, not just steady-state ground running.
inline Recording jump_arc_snap_correction() {
	Recording rec;
	rec.world_plane_y = 0.0f;
	rec.start = MovementSnapshot{};

	const float dt   = static_cast<float>(movement::kTickSeconds);
	const float step = movement::kRunSpeed * dt;

	std::vector<MovementInput> inputs;
	// 4 forward-run ticks on the ground.
	for (int i = 0; i < 4; ++i) inputs.push_back(fwd_run());
	// Tick 5: jump while running forward (launches the gravity arc).
	{
		MovementInput j = fwd_run();
		j.jump = true;
		inputs.push_back(j);
	}
	// 9 more forward-run ticks: the airborne arc integrates and lands.
	for (int i = 0; i < 9; ++i) inputs.push_back(fwd_run());

	for (std::size_t i = 0; i < inputs.size(); ++i) {
		ReplayEvent ev;
		ev.client_time_ms = static_cast<uint64_t>(i) * 50;
		ev.input = inputs[i];

		// A LARGE correction at tick 12 (index 11): server snaps us 3 m sideways —
		// well over kSnapThresholdMeters (1.0) -> snap, offset dropped to zero.
		if (i == 11) {
			ev.has_correction         = true;
			ev.correction.ack_seq     = 8;
			ev.correction.position.x  = 3.0f;   // > snap threshold
			ev.correction.position.y  = 0.0f;
			ev.correction.position.z  = step * 8.0f;
			ev.correction.server_time_ms = ev.client_time_ms;
			ev.advance_ms             = 50;
		}
		rec.events.push_back(ev);
	}
	return rec;
}

// The catalogue of checked-in fixtures: (filename, recording). The generator writes
// each; the test loads each and asserts the round-trip + golden replay.
struct FixtureDef {
	std::string filename;
	std::string name;
	Recording   recording;
};

inline std::vector<FixtureDef> all_fixtures() {
	return {
	    {"forward_run_small_correction.replay", "forward_run_small_correction",
	     forward_run_small_correction()},
	    {"jump_arc_snap_correction.replay", "jump_arc_snap_correction",
	     jump_arc_snap_correction()},
	};
}

} // namespace meridian::replay::fixtures

#endif // MERIDIAN_REPLAY_FIXTURE_DEFS_H
