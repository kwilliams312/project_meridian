// Project Meridian — engine-free unit test for prediction RECONCILIATION
// SMOOTHING (issue #103). NO Godot: it compiles against the plain-C++ core
// (movement_controller.*) + the #101 headers only, so it runs in any C++17
// toolchain without a Godot runtime (Client SAD §9.2 engine-agnostic cores).
// Plain-main style, mirroring the #102 test (movement_controller_test.cpp) and the
// server tests; ctest-wired via test/CMakeLists.txt.
//
// #102 already proved the ring buffer + rewind/re-simulate (the SIM correctness).
// THIS test pins the #103 net-new: the error-offset PRESENTATION layer on top of
// that corrected sim (Client SAD §2.2 (b) "smooth small errors, snap large ones"):
//   1. RING BUFFER      — predict N inputs -> buffer holds all N (seq/input/state).
//   2. NO-OP RECONCILE  — matching server state -> zero error, no smoothing.
//   3. SMALL ERROR      — sub-threshold delta -> visible stays continuous, then the
//                         offset decays MONOTONICALLY to EXACTLY zero over the
//                         100–200 ms window (converges); visible -> corrected sim.
//   4. LARGE ERROR      — over-threshold delta -> SNAP (offset 0 immediately,
//                         visible == corrected sim, is_smoothing() false).
//   5. ACK DROP + RESIM — acked inputs (seq <= ack) dropped; unacked re-simulated
//                         in order (corrected sim = server + local residual).
//   6. DETERMINISM      — same inputs + same server state -> same corrected state
//                         AND same error offset (bit-reproducible).

#include "movement_constants.h"
#include "movement_controller.h"
#include "movement_query.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace mv = meridian::movement;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

static bool near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps;
}

static float mag(const mv::Vec3& v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// A forward-run input on the ground (unit forward, run mode).
static mv::MovementInput fwd_run() {
	mv::MovementInput in;
	in.move_z = 1.0f;   // forward
	in.walk   = false;  // run
	return in;
}

// Predict `k` forward-run ticks into a fresh reconciler, then reconcile against a
// server state that acks `m` ticks and reports (server_x, server_z). Returns the
// reconciler by out-params so callers can inspect it. Shared by several cases +
// the determinism replay so both runs are byte-identical.
static void run_scenario(int k, int m, float server_x, float server_z,
                         mv::PredictionReconciler& rec) {
	for (int i = 0; i < k; ++i) {
		rec.predict(fwd_run(), static_cast<uint64_t>(i) * 50);
	}
	mv::MovementStateIn server;
	server.ack_seq    = static_cast<uint32_t>(m);
	server.position.x = server_x;
	server.position.z = server_z;
	server.position.y = 0.0f;
	rec.reconcile(server);
}

int main() {
	std::printf("meridian prediction reconciliation smoothing test (#103)\n");
	const float dt = static_cast<float>(mv::kTickSeconds);
	const float step = mv::kRunSpeed * dt;   // per-tick forward-run advance (0.3 m)

	// =======================================================================
	// 0. Tunables are documented + within the #103 envelope (no magic numbers).
	// =======================================================================
	std::printf("[0] tunables in range\n");
	{
		check("decay window in 100-200 ms envelope",
		      mv::reconcile_tuning::kErrorDecayWindowMs >= 100 &&
		          mv::reconcile_tuning::kErrorDecayWindowMs <= 200);
		check("snap threshold positive + above per-packet jitter",
		      mv::reconcile_tuning::kSnapThresholdMeters > mv::kMaxPacketDisplacement);
	}

	// =======================================================================
	// 1. RING BUFFER — predict N inputs -> buffer holds them.
	// =======================================================================
	std::printf("[1] ring buffer holds predicted inputs\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);
		const int N = 12;
		for (int i = 0; i < N; ++i) rec.predict(fwd_run(), static_cast<uint64_t>(i) * 50);
		check("N inputs buffered", rec.pending_count() == static_cast<std::size_t>(N));
		check("last_seq == N", rec.last_seq() == static_cast<uint32_t>(N));
		check("predicted z == N run ticks", near(rec.predicted_state().position.z, step * N));
		check("no smoothing before any reconcile", !rec.is_smoothing());
		check("zero error offset before any reconcile", near(mag(rec.error_offset()), 0.0f));
	}

	// =======================================================================
	// 2. NO-OP RECONCILE — server agrees exactly -> no correction, no smoothing.
	// =======================================================================
	std::printf("[2] matching server state -> no correction\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);
		const int K = 6, M = 2;
		// Server reports EXACTLY what the client predicted for the acked ticks.
		run_scenario(K, M, /*server_x=*/0.0f, /*server_z=*/step * M, rec);
		check("no-op reconcile: zero error magnitude", near(rec.last_error_magnitude(), 0.0f));
		check("no-op reconcile: not smoothing", !rec.is_smoothing());
		check("no-op reconcile: not snapped", !rec.last_reconcile_snapped());
		check("no-op reconcile: zero offset", near(mag(rec.error_offset()), 0.0f));
		// Visible position == sim position when nothing is being smoothed.
		check("visible == sim (no offset)",
		      near(rec.visible_state().position.z, rec.predicted_state().position.z) &&
		          near(rec.visible_state().position.x, rec.predicted_state().position.x));
	}

	// =======================================================================
	// 3. SMALL ERROR — sub-threshold correction decays smoothly to zero.
	// =======================================================================
	std::printf("[3] small error: monotonic decay to zero over the window\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);
		const int K = 8, M = 3;
		const float nudge = 0.2f;   // < kSnapThresholdMeters (1.0) -> smoothed
		// Client predicted x=0; server nudges x by +0.2 (a small lateral correction).
		const float pre_x = rec.predicted_state().position.x;  // 0 before predicting
		(void)pre_x;
		run_scenario(K, M, /*server_x=*/nudge, /*server_z=*/step * M, rec);

		// Corrected SIM: server correction preserved in x, z = server + re-sim residual.
		check("acked inputs dropped", rec.pending_count() == static_cast<std::size_t>(K - M));
		check("corrected sim x == server nudge", near(rec.predicted_state().position.x, nudge));
		check("corrected sim z == server + residual",
		      near(rec.predicted_state().position.z, step * M + step * (K - M)));

		// Smoothing engaged (small error), NOT snapped.
		check("small error -> smoothing active", rec.is_smoothing());
		check("small error -> not snapped", !rec.last_reconcile_snapped());
		check("error magnitude == nudge", near(rec.last_error_magnitude(), nudge));

		// CONTINUITY: at t=0 the visible position equals where we WERE showing the
		// player (x=0), i.e. corrected_sim + offset, so there is no visible pop.
		check("visible continuous at correction (x still 0)",
		      near(rec.visible_state().position.x, 0.0f));
		check("initial offset magnitude == nudge", near(mag(rec.error_offset()), nudge));

		// Advance the decay in fixed 10 ms steps and record the offset magnitude.
		// Assert it is MONOTONICALLY non-increasing, STRICTLY decreasing while the
		// window is open, and reaches EXACTLY zero at/after the window end.
		std::vector<float> mags;
		mags.push_back(mag(rec.error_offset()));  // t=0
		const uint64_t stepMs = 10;
		const uint64_t window = mv::reconcile_tuning::kErrorDecayWindowMs;
		bool monotonic = true, strictly_dec_in_window = true;
		bool visible_converges_monotonic = true;
		float prev_vis_x = rec.visible_state().position.x;  // starts at 0, target = nudge
		for (uint64_t t = stepMs; t <= window + 40; t += stepMs) {
			rec.advance_smoothing(stepMs);
			const float m = mag(rec.error_offset());
			const float prev = mags.back();
			if (m > prev + 1e-6f) monotonic = false;
			// Strictly decreasing while still inside the window and still non-zero.
			if (t <= window && prev > 1e-6f && !(m < prev - 1e-7f)) {
				strictly_dec_in_window = false;
			}
			// Visible x should climb monotonically from 0 toward the corrected nudge.
			const float vis_x = rec.visible_state().position.x;
			if (vis_x < prev_vis_x - 1e-6f) visible_converges_monotonic = false;
			prev_vis_x = vis_x;
			mags.push_back(m);
		}
		check("offset decay is monotonic (non-increasing)", monotonic);
		check("offset strictly decreases while window open", strictly_dec_in_window);
		check("offset converges to exactly zero after the window", near(mags.back(), 0.0f, 1e-6f));
		check("smoothing inactive after the window", !rec.is_smoothing());
		check("visible converges monotonically toward corrected sim",
		      visible_converges_monotonic);
		check("visible == corrected sim after the window",
		      near(rec.visible_state().position.x, nudge) &&
		          near(rec.visible_state().position.z, rec.predicted_state().position.z));
	}

	// =======================================================================
	// 3b. Decay is FRAME-RATE INDEPENDENT — one big step == many small steps.
	// =======================================================================
	// Because the curve is time-parameterised (scale = f(elapsed)), reaching the
	// same elapsed time via different step sizes yields the SAME offset.
	std::printf("[3b] decay is frame-rate independent\n");
	{
		mv::FlatWorldQuery wa(0.0f), wb(0.0f);
		mv::MovementSnapshot sa, sb;
		mv::PredictionReconciler ra(wa, sa), rb(wb, sb);
		run_scenario(8, 3, 0.2f, 0.9f, ra);
		run_scenario(8, 3, 0.2f, 0.9f, rb);
		// ra: one 60 ms step. rb: six 10 ms steps. Same elapsed (60 ms).
		ra.advance_smoothing(60);
		for (int i = 0; i < 6; ++i) rb.advance_smoothing(10);
		check("coarse vs fine stepping give the same offset",
		      near(mag(ra.error_offset()), mag(rb.error_offset()), 1e-6f));
	}

	// =======================================================================
	// 4. LARGE ERROR — over-threshold correction SNAPS immediately.
	// =======================================================================
	std::printf("[4] large error: snap immediately\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);
		const int K = 8, M = 3;
		const float jump = 3.0f;   // > kSnapThresholdMeters (1.0) -> snap
		run_scenario(K, M, /*server_x=*/jump, /*server_z=*/step * M, rec);
		check("large error -> snapped", rec.last_reconcile_snapped());
		check("large error -> not smoothing", !rec.is_smoothing());
		check("large error -> zero offset (snapped straight to sim)",
		      near(mag(rec.error_offset()), 0.0f));
		check("error magnitude recorded (~jump)", rec.last_error_magnitude() > jump - 0.1f);
		// Visible position == corrected sim immediately (no glide).
		check("visible == corrected sim on snap",
		      near(rec.visible_state().position.x, rec.predicted_state().position.x) &&
		          near(rec.visible_state().position.z, rec.predicted_state().position.z));
		check("corrected sim x == server (snap target)",
		      near(rec.predicted_state().position.x, jump));
	}

	// =======================================================================
	// 4b. Boundary: error just BELOW the snap threshold smooths (does not snap).
	// =======================================================================
	std::printf("[4b] just-below-threshold error smooths\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);
		const float below = mv::reconcile_tuning::kSnapThresholdMeters - 0.05f;
		run_scenario(8, 3, /*server_x=*/below, /*server_z=*/0.9f, rec);
		check("just-below threshold -> smoothing (not snap)",
		      rec.is_smoothing() && !rec.last_reconcile_snapped());
	}

	// =======================================================================
	// 5. ACK DROP + RESIM ORDER — unacked inputs re-simulated in sequence order.
	// =======================================================================
	// Mixed inputs: forward-run then a WALK segment. After the server acks the
	// first M, the surviving (K-M) inputs must be replayed IN ORDER from the
	// authoritative position — replaying out of order would give a different z.
	std::printf("[5] acked drop + ordered re-simulation of unacked\n");
	{
		mv::FlatWorldQuery world(0.0f);
		mv::MovementSnapshot start;
		mv::PredictionReconciler rec(world, start);
		// 4 run ticks then 4 walk ticks (distinct per-tick advances -> order matters).
		std::vector<mv::MovementInput> inputs;
		for (int i = 0; i < 4; ++i) inputs.push_back(fwd_run());
		for (int i = 0; i < 4; ++i) {
			mv::MovementInput w = fwd_run();
			w.walk = true;
			inputs.push_back(w);
		}
		for (std::size_t i = 0; i < inputs.size(); ++i) {
			rec.predict(inputs[i], i * 50);
		}
		const int M = 2;  // ack first two (run) ticks
		mv::MovementStateIn server;
		server.ack_seq    = static_cast<uint32_t>(M);
		server.position.x = 0.0f;
		server.position.z = mv::kRunSpeed * dt * M;   // server agrees on the 2 run ticks
		server.position.y = 0.0f;
		rec.reconcile(server);

		check("survivors == total - acked", rec.pending_count() == inputs.size() - M);
		// Corrected z: server z (2 run) + 2 more run ticks + 4 walk ticks, IN ORDER.
		const float expected_z = mv::kRunSpeed * dt * M            // server-acked (2 run)
		                       + mv::kRunSpeed * dt * 2            // remaining 2 run ticks
		                       + mv::kWalkSpeed * dt * 4;          // 4 walk ticks
		check("ordered re-sim reproduces mixed-speed residual",
		      near(rec.predicted_state().position.z, expected_z));
		check("server-agreeing reconcile -> no smoothing",
		      !rec.is_smoothing() && near(rec.last_error_magnitude(), 0.0f));
	}

	// =======================================================================
	// 6. DETERMINISM — identical inputs + server state -> identical outcome.
	// =======================================================================
	// The property-ish check: two independent reconcilers driven with the SAME
	// sequence produce bit-identical corrected sim AND bit-identical error offset,
	// including through the decay (the reconciliation must be reproducible so the
	// server track / re-simulation agree — Client SAD R5).
	std::printf("[6] determinism: same inputs+server -> same corrected state+offset\n");
	{
		mv::FlatWorldQuery wa(0.0f), wb(0.0f);
		mv::MovementSnapshot sa, sb;
		mv::PredictionReconciler ra(wa, sa), rb(wb, sb);
		run_scenario(10, 4, /*server_x=*/0.3f, /*server_z=*/1.2f, ra);
		run_scenario(10, 4, /*server_x=*/0.3f, /*server_z=*/1.2f, rb);

		check("corrected sim identical (x)",
		      near(ra.predicted_state().position.x, rb.predicted_state().position.x, 0.0f));
		check("corrected sim identical (z)",
		      near(ra.predicted_state().position.z, rb.predicted_state().position.z, 0.0f));
		check("error offset identical", near(mag(ra.error_offset()), mag(rb.error_offset()), 0.0f));
		check("error magnitude identical",
		      near(ra.last_error_magnitude(), rb.last_error_magnitude(), 0.0f));

		// Advance both by the same schedule -> still bit-identical.
		for (int i = 0; i < 20; ++i) { ra.advance_smoothing(7); rb.advance_smoothing(7); }
		check("offsets identical after identical decay schedule",
		      near(ra.error_offset().x, rb.error_offset().x, 0.0f) &&
		          near(ra.error_offset().z, rb.error_offset().z, 0.0f));
		check("visible states identical after decay",
		      near(ra.visible_state().position.x, rb.visible_state().position.x, 0.0f) &&
		          near(ra.visible_state().position.z, rb.visible_state().position.z, 0.0f));
	}

	std::printf(g_fail == 0 ? "\nALL PREDICTION RECONCILE TESTS PASSED\n"
	                        : "\n%d PREDICTION RECONCILE TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
