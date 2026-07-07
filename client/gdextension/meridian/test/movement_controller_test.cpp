// Project Meridian — engine-free unit test for the kinematic movement
// controller core (issue #102). NO Godot: it compiles against the plain-C++
// core (movement_controller.*) + the #101 headers only, so it runs in any
// C++17 toolchain without a Godot runtime (Client SAD §9.2 engine-agnostic
// cores; §11 client doctest). Plain-main style, mirroring the server tests
// (server/worldd/test/*), ctest-wired via client/gdextension/meridian/test/
// CMakeLists.txt.
//
// Proves the three #102 deliverables (docs/movement-spike.md §3):
//   1. INTEGRATOR — advances position at the LOCKED speed over N ticks,
//      deterministically and exactly.
//   2. RECONCILIATION — given a server correction, re-simulating unacked inputs
//      converges to server-authoritative + local residual (the SAD R5 / R3
//      cross-track determinism check; a GOLDEN fixture pins the vectors).
//   3. INTENT EMISSION — respects the ≤ 10/s + on-state-change rate cap.

#include "movement_constants.h"
#include "movement_controller.h"
#include "movement_query.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace mv = meridian::movement;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

// Float compare with an absolute epsilon (deterministic exact-ish math).
static bool near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps;
}

// A forward-run input on the ground (unit forward, run mode).
static mv::MovementInput fwd_run(float orientation = 0.0f) {
	mv::MovementInput in;
	in.move_z      = 1.0f;   // forward
	in.walk        = false;  // run
	in.orientation = orientation;
	return in;
}

int main() {
	std::printf("meridian movement controller core test (#102)\n");
	const float dt = static_cast<float>(mv::kTickSeconds);

	// =======================================================================
	// 1. INTEGRATOR — deterministic advance at the LOCKED run speed.
	// =======================================================================
	// On the flat M0 map (FlatWorldQuery, y=0), N ticks of unit-forward run must
	// advance +z by exactly kRunSpeed * dt each tick, y pinned to 0, grounded.
	std::printf("[1] integrator: constant-speed advance\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot s;  // origin, grounded
		const int N = 20;        // 1 second at 20 Hz
		for (int i = 0; i < N; ++i) {
			s = mv::integrate_tick(s, fwd_run(), world);
		}
		const float expected_z = mv::kRunSpeed * dt * N;   // 6.0 * 0.05 * 20 = 6.0 m
		check("z advanced run_speed*dt*N exactly", near(s.position.z, expected_z));
		check("x unchanged (pure forward)", near(s.position.x, 0.0f));
		check("y pinned to ground plane", near(s.position.y, 0.0f));
		check("grounded after ground run", s.grounded);
		check("velocity.z == run speed", near(s.velocity.z, mv::kRunSpeed));
		// Determinism: replaying the SAME inputs yields the SAME state.
		mv::MovementSnapshot s2;
		for (int i = 0; i < N; ++i) s2 = mv::integrate_tick(s2, fwd_run(), world);
		check("deterministic replay reproduces state",
		      near(s2.position.z, s.position.z) && near(s2.position.x, s.position.x) &&
		          near(s2.position.y, s.position.y));
	}

	// Walk cap is slower than run (locked constants).
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot s;
		mv::MovementInput walk = fwd_run();
		walk.walk = true;
		s = mv::integrate_tick(s, walk, world);
		check("walk step == walk_speed*dt", near(s.position.z, mv::kWalkSpeed * dt));
		check("walk mode resolved", s.mode == mv::MoveMode::Walk);
	}

	// Diagonal input is normalized (no faster-than-run diagonal).
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot s;
		mv::MovementInput diag = fwd_run();
		diag.move_x = 1.0f;  // forward + right
		s = mv::integrate_tick(s, diag, world);
		const float step = std::sqrt(s.position.x * s.position.x +
		                             s.position.z * s.position.z);
		check("diagonal speed clamped to run", near(step, mv::kRunSpeed * dt));
	}

	// Jump: leaves the ground, gravity brings it back, lands near origin y.
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot s;
		mv::MovementInput jump;  // no horizontal, jump=true
		jump.jump = true;
		s = mv::integrate_tick(s, jump, world);
		check("jump leaves ground", !s.grounded && s.velocity.y > 0.0f);
		// Integrate until it lands again (bounded loop).
		mv::MovementInput coast;  // no input while airborne
		bool landed = false;
		for (int i = 0; i < 200 && !landed; ++i) {
			s = mv::integrate_tick(s, coast, world);
			if (s.grounded) landed = true;
		}
		check("jump lands back on ground", landed && near(s.position.y, 0.0f));
	}

	// =======================================================================
	// 2. RECONCILIATION — golden cross-track fixture (SAD R5 / R3).
	// =======================================================================
	// Client predicts K forward-run ticks (K unacked). Server acks the first M
	// of them but reports an authoritative position that DIFFERS from the
	// client's prediction (a correction / snap-back — e.g. a nudge in x). After
	// reconcile(ack=M, server_pos), re-simulating the (K-M) surviving inputs from
	// the server position must yield:
	//     server_pos + (residual of the K-M unacked forward-run ticks)
	// i.e. the correction is preserved AND the local unacked motion is replayed
	// on top of it — smooth reconciliation, not a hard reset to the stale server
	// position (Client SAD §2.2 (b)).
	std::printf("[2] reconciliation: correction + re-sim of unacked inputs\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;  // origin
		mv::PredictionReconciler rec(world, start);

		const int K = 10;   // total predicted ticks
		const int M = 4;    // server has acked seq 1..M
		for (int i = 0; i < K; ++i) {
			rec.predict(fwd_run(), /*client_time_ms=*/static_cast<uint64_t>(i) * 50);
		}
		check("K inputs buffered before ack", rec.pending_count() == (std::size_t)K);
		const mv::MovementSnapshot pre = rec.predicted_state();
		check("pre-reconcile z == K run ticks",
		      near(pre.position.z, mv::kRunSpeed * dt * K));

		// GOLDEN FIXTURE: authoritative state at seq=M. The server agrees on z
		// for M ticks but CORRECTS x by +0.5 m (a lateral nudge the client did
		// not predict) — the snap-back the reconciler must preserve.
		mv::MovementStateIn server;
		server.ack_seq     = (uint32_t)M;
		server.position.x  = 0.5f;                       // correction (client had 0)
		server.position.z  = mv::kRunSpeed * dt * M;     // server z for M ticks
		server.position.y  = 0.0f;
		server.orientation = 0.0f;

		const mv::MovementSnapshot after = rec.reconcile(server);

		// Only the unacked (K-M) inputs remain.
		check("acked inputs discarded", rec.pending_count() == (std::size_t)(K - M));

		// EXPECTED (golden): server correction preserved in x; z is server z plus
		// the (K-M) re-simulated forward-run ticks.
		const float expected_x = 0.5f;                              // correction kept
		const float expected_z = mv::kRunSpeed * dt * M             // server-acked z
		                       + mv::kRunSpeed * dt * (K - M);       // re-sim residual
		check("reconciled x preserves server correction",
		      near(after.position.x, expected_x));
		check("reconciled z == server + re-simulated residual",
		      near(after.position.z, expected_z));
		check("reconciled y on ground", near(after.position.y, 0.0f));

		// Cross-track determinism: reconciling to a state the client ALREADY
		// predicted exactly (no correction) is a no-op on position — the golden
		// invariant the server track (#86) replays.
		mv::FlatWorldQuery world2(0.0f);
		mv::MovementSnapshot start2;
		mv::PredictionReconciler rec2(world2, start2);
		for (int i = 0; i < K; ++i) rec2.predict(fwd_run(), (uint64_t)i * 50);
		mv::MovementStateIn agree;
		agree.ack_seq    = (uint32_t)M;
		agree.position.x = 0.0f;                       // server AGREES (client had 0)
		agree.position.z = mv::kRunSpeed * dt * M;
		agree.position.y = 0.0f;
		const mv::MovementSnapshot noop = rec2.reconcile(agree);
		check("no-correction reconcile is position-preserving",
		      near(noop.position.z, mv::kRunSpeed * dt * K) &&
		          near(noop.position.x, 0.0f));
	}

	// =======================================================================
	// 3. INTENT EMISSION — ≤ 10/s + on-state-change rate cap.
	// =======================================================================
	std::printf("[3] intent emission: rate cap\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);

		const uint32_t running = mv::flags::kForward;  // steady running flags

		// First emit always allowed.
		check("first intent emits", rec.should_emit_intent(0, running));
		// Same flags, only 50 ms later (< 100 ms cap) -> suppressed.
		check("50ms same-flags suppressed", !rec.should_emit_intent(50, running));
		// 100 ms after the last SEND -> allowed (10/s cap).
		check("100ms elapsed emits", rec.should_emit_intent(100, running));
		// 40 ms later but a STATE CHANGE (add jump) -> allowed (on-change clause).
		check("state change emits despite cap",
		      rec.should_emit_intent(140, running | mv::flags::kJump));
		// Immediately after, same new flags, < 100 ms -> suppressed.
		check("post-change same-flags suppressed",
		      !rec.should_emit_intent(150, running | mv::flags::kJump));

		// Over a 1 s window of 20 sim ticks (50 ms each) with UNCHANGING flags,
		// at most 10 intents should pass the gate (the ≤ 10/s guarantee).
		mv::FlatWorldQuery w2(0.0f);
		mv::MovementSnapshot st2;
		mv::PredictionReconciler rec2(w2, st2);
		int emitted = 0;
		for (int tick = 0; tick < 20; ++tick) {
			if (rec2.should_emit_intent((uint64_t)tick * 50, running)) ++emitted;
		}
		check("<=10 intents over 1s of 20 ticks (steady flags)", emitted <= 10);
		check(">=10 intents over 1s (cap actually reached)", emitted >= 10);

		// state_flags encoding round-trips the movement bits.
		mv::MovementInput fwd = fwd_run();
		uint32_t f = mv::encode_state_flags(fwd, mv::MoveMode::Run);
		check("forward encodes kForward bit", (f & mv::flags::kForward) != 0);
		check("forward not walk bit", (f & mv::flags::kWalk) == 0);
	}

	// =======================================================================
	// 4. CANONICAL state_flags ENCODING (#247) — cross-track round-trip.
	// =======================================================================
	// The client encodes the active MoveMode into the LOW 3 BITS (the field the
	// server #86 reads with `state_flags & 0x7`), with direction/jump/walk flags
	// ABOVE them. This test asserts:
	//   (a) the low 3 bits equal the MoveMode value the client passed;
	//   (b) mode_from_state_flags (the shared decode the server's mode_from_flags
	//       delegates to) recovers that SAME mode from the client's encoding — the
	//       encode→decode round-trip both build trees must agree on (movement-
	//       spike.md §4 discipline, mirrored on the server track);
	//   (c) the direction/jump/walk flags survive ABOVE the mode field intact.
	std::printf("[4] canonical state_flags encoding: cross-track round-trip\n");
	{
		struct Combo { mv::MoveMode mode; float mx; float mz; bool jump; bool walk; };
		const Combo combos[] = {
		    {mv::MoveMode::Idle, 0.0f, 0.0f, false, false},
		    {mv::MoveMode::Run,  0.0f, 1.0f, false, false},   // forward run
		    {mv::MoveMode::Run,  0.0f, -1.0f, false, false},  // backpedal
		    {mv::MoveMode::Walk, 1.0f, 0.0f, false, true},    // strafe-right walk
		    {mv::MoveMode::Run,  -1.0f, 1.0f, false, false},  // fwd + strafe-left
		    {mv::MoveMode::Jump, 0.0f, 1.0f, true, false},    // forward jump
		};
		int rt_ok = 0;
		const int n = static_cast<int>(sizeof(combos) / sizeof(combos[0]));
		for (const Combo& c : combos) {
			mv::MovementInput in;
			in.move_x = c.mx;
			in.move_z = c.mz;
			in.jump   = c.jump;
			in.walk   = c.walk;
			const uint32_t enc = mv::encode_state_flags(in, c.mode);

			// (a) low 3 bits == the mode value.
			const bool low3_is_mode =
			    (enc & mv::kStateFlagsModeMask) == static_cast<uint32_t>(c.mode);
			// (b) shared decode recovers the same mode.
			const bool decodes_back = mv::mode_from_state_flags(enc) == c.mode;
			// (c) direction/jump/walk flags land ABOVE the mode bits, intact.
			bool dirs_ok = true;
			if (c.mz > 0.0f) dirs_ok = dirs_ok && (enc & mv::flags::kForward);
			if (c.mz < 0.0f) dirs_ok = dirs_ok && (enc & mv::flags::kBack);
			if (c.mx > 0.0f) dirs_ok = dirs_ok && (enc & mv::flags::kStrafeR);
			if (c.mx < 0.0f) dirs_ok = dirs_ok && (enc & mv::flags::kStrafeL);
			if (c.jump)      dirs_ok = dirs_ok && (enc & mv::flags::kJump);
			if (c.walk)      dirs_ok = dirs_ok && (enc & mv::flags::kWalk);
			// The direction flags must NOT bleed into the low 3 mode bits.
			const bool no_dir_in_mode =
			    ((mv::flags::kForward | mv::flags::kBack | mv::flags::kStrafeL |
			      mv::flags::kStrafeR | mv::flags::kJump | mv::flags::kWalk) &
			     mv::kStateFlagsModeMask) == 0u;

			if (low3_is_mode && decodes_back && dirs_ok && no_dir_in_mode) ++rt_ok;
		}
		check("all mode+direction combos round-trip encode->decode", rt_ok == n);

		// Static-ish layout check: the direction bit block and the mode mask are
		// disjoint (the compile-time contract both build trees encode against).
		check("direction flags are disjoint from the mode mask",
		      ((mv::flags::kForward | mv::flags::kBack | mv::flags::kStrafeL |
		        mv::flags::kStrafeR | mv::flags::kJump | mv::flags::kWalk) &
		       mv::kStateFlagsModeMask) == 0u);
	}

	// =======================================================================
	// 5. RENDER-POSITION ADVANCE ON INPUT (#303) — the on-screen WASD path.
	// =======================================================================
	// The decisive regression for #303 "WASD does not move the local player": the
	// networked world scene (scenes/world/world.gd) renders the local capsule from
	// get_render_position() == visible_state().position — NOT get_predicted_position().
	// Every prior case starts at the ORIGIN and mostly asserts predicted_state();
	// none pins that a BARE predict() (no server reconcile) from a NON-ORIGIN spawn
	// advances the VISIBLE position the capsule is actually drawn at. This mirrors
	// world.gd exactly: reset(SPAWN) -> per tick build the world-space move from
	// WASD + camera yaw -> predict() -> advance_smoothing() -> get_render_position().
	std::printf("[5] render position advances on input (#303 WASD path)\n");
	{
		// world.gd SPAWN = Godot (x=64, y=0, z=64), grounded on the y=0 flat map.
		const mv::Vec3 spawn{64.0f, 0.0f, 64.0f};
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		start.position = spawn;
		start.grounded = true;
		mv::PredictionReconciler rec(world, start);

		// With no predict/reconcile yet, the RENDER position IS the spawn.
		const mv::MovementSnapshot v0 = rec.visible_state();
		check("render position starts at spawn (x)", near(v0.position.x, spawn.x));
		check("render position starts at spawn (z)", near(v0.position.z, spawn.z));

		// world.gd input->world-move mapping (yaw=0 => forward is -Z; Godot/WoW).
		// Press W: fwd=+1, strafe=0, yaw=0 -> move=(0,0,-1).
		const float yaw = 0.0f;
		const float sin_y = std::sin(yaw), cos_y = std::cos(yaw);
		const float fwd = 1.0f, strafe = 0.0f;
		mv::MovementInput press_w;
		press_w.move_x = strafe * cos_y - (-fwd) * sin_y;   // == 0
		press_w.move_z = strafe * sin_y + (-fwd) * cos_y;   // == -1
		press_w.orientation = yaw;

		// ONE W tick: predict, then advance_smoothing (world.gd step 4) — with no
		// server reconcile pending the smoothing pass is a no-op and MUST NOT undo
		// the predicted advance.
		rec.predict(press_w, /*client_time_ms=*/50);
		rec.advance_smoothing(16);   // ~one 60 fps render frame
		const mv::MovementSnapshot v1 = rec.visible_state();

		const float dz = v1.position.z - spawn.z;
		const float disp1 = std::sqrt(
		    (v1.position.x - spawn.x) * (v1.position.x - spawn.x) +
		    (v1.position.z - spawn.z) * (v1.position.z - spawn.z));
		check("one W tick moves the RENDER position away from spawn (nonzero)",
		      disp1 > 1e-3f);
		check("W moves the render position toward -Z (Godot forward)", dz < 0.0f);
		check("render position tracks the predicted sim with no reconcile",
		      near(v1.position.x, rec.predicted_state().position.x) &&
		          near(v1.position.z, rec.predicted_state().position.z));

		// Holding W advances the render position monotonically away from spawn over
		// many ticks — the sustained on-screen motion #303 reports as missing.
		float prev_dist = disp1;
		bool monotonic_away = true;
		for (int i = 2; i <= 20; ++i) {
			rec.predict(press_w, static_cast<uint64_t>(i) * 50);
			rec.advance_smoothing(16);
			const mv::MovementSnapshot v = rec.visible_state();
			const float d = std::sqrt(
			    (v.position.x - spawn.x) * (v.position.x - spawn.x) +
			    (v.position.z - spawn.z) * (v.position.z - spawn.z));
			if (d <= prev_dist) monotonic_away = false;
			prev_dist = d;
		}
		check("holding W keeps advancing the render position away from spawn",
		      monotonic_away && prev_dist > disp1);

		// A different key moves the render position on the OTHER axis: press D
		// (strafe right) from a fresh spawn -> render x increases.
		mv::PredictionReconciler recd(world, start);
		mv::MovementInput press_d;
		press_d.move_x = 1.0f * cos_y - 0.0f * sin_y;   // strafe=+1 -> world_x == 1
		press_d.move_z = 1.0f * sin_y + 0.0f * cos_y;   // == 0
		press_d.orientation = yaw;
		recd.predict(press_d, 50);
		recd.advance_smoothing(16);
		const mv::MovementSnapshot vd = recd.visible_state();
		check("D moves the render position toward +X (nonzero)",
		      (vd.position.x - spawn.x) > 1e-3f);
	}

	std::printf(g_fail == 0 ? "\nALL MOVEMENT CONTROLLER TESTS PASSED\n"
	                        : "\n%d MOVEMENT CONTROLLER TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
