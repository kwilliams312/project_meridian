// Project Meridian — engine-free unit test for the remote-entity interpolation
// + clock-sync estimator core (issue #104). NO Godot: compiles against the
// plain-C++ core (remote_interpolation.*) + the #102 headers only, so it runs in
// any C++17 toolchain (Client SAD §9.2 engine-agnostic cores; §11 client
// doctest). Plain-main style, mirroring the #102 movement_controller_test.
//
// Time + snapshots are INJECTED — no wall clock is read — so every assertion is
// deterministic. Proves the four #104 deliverables:
//   (a) INTERPOLATION      — snapshots at t=0 (x=0) and t=100ms (x=10) render the
//                            exact midpoint at the delayed render cursor.
//   (b) JITTER ABSORPTION  — out-of-order / jittery arrival still renders
//                            monotonically with no teleport.
//   (c) ENTER/LEAVE/GAP    — lifecycle + extrapolation cap + freeze.
//   (d) CLOCK SYNC         — round-trips with a known offset + noise converge to
//                            the offset within tolerance and reject outliers.

#include "movement_controller.h"      // Vec3
#include "remote_interpolation.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace rem = meridian::remote;
using rem::Vec3;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

static bool near(float a, float b, float eps = 1e-3f) {
	return std::fabs(a - b) <= eps;
}
static bool near_i(int64_t a, int64_t b, int64_t tol) {
	int64_t d = a - b;
	if (d < 0) d = -d;
	return d <= tol;
}

int main() {
	std::printf("meridian remote interpolation + clock-sync core test (#104)\n");

	// =======================================================================
	// (a) INTERPOLATION — the midpoint check the issue names explicitly.
	// =======================================================================
	// Two snapshots: t=0 (x=0) and t=100ms (x=10). Sampling the buffer at server
	// time 50ms must yield exactly x=5 (the midpoint). This is the raw buffer
	// query (delay already applied by the caller).
	std::printf("[a] interpolation: exact midpoint between two snapshots\n");
	{
		rem::SnapshotBuffer buf;
		buf.push(rem::Snapshot{0,   Vec3{0.0f, 0.0f, 0.0f}, 0.0f});
		buf.push(rem::Snapshot{100, Vec3{10.0f, 0.0f, 0.0f}, 0.0f});

		auto at50 = buf.sample(50);
		check("kind == interpolated", at50.kind == rem::SampleKind::kInterpolated);
		check("x == 5.0 (midpoint)", near(at50.position.x, 5.0f));

		// Quarter + three-quarter points along the segment.
		check("x(25ms) == 2.5", near(buf.sample(25).position.x, 2.5f));
		check("x(75ms) == 7.5", near(buf.sample(75).position.x, 7.5f));

		// Exact endpoints.
		check("x(0ms) == 0.0",   near(buf.sample(0).position.x, 0.0f));
		check("x(100ms) == 10.0", near(buf.sample(100).position.x, 10.0f));
	}

	// The same check through the FULL facade, exercising the interp-delay math.
	// With a zero clock offset, render cursor = client_now - 100ms delay. Push
	// snapshots at server t=0 and t=100; sampling at client_now=150 renders at
	// server 50 → midpoint x=5.
	std::printf("[a2] interpolation: through the facade + interp-delay cursor\n");
	{
		rem::RemoteInterpolator interp;   // no clock samples → offset 0 (identity)
		interp.on_enter(42, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, /*server_t=*/0);
		interp.on_update(42, Vec3{10.0f, 0.0f, 0.0f}, 0.0f, /*server_t=*/100);

		check("render cursor(150) == 50", interp.render_server_time(150) == 50);
		auto s = interp.sample_entity(42, /*client_now=*/150);
		check("facade kind interpolated", s.kind == rem::SampleKind::kInterpolated);
		check("facade x == 5.0 midpoint", near(s.position.x, 5.0f));
	}

	// =======================================================================
	// (b) JITTER ABSORPTION — out-of-order + jittery arrival still smooth.
	// =======================================================================
	// A remote entity moving +x at constant speed emits snapshots every 50ms:
	// t=0..250 with x=0,2,4,6,8,10. We PUSH them in a jittered/out-of-order order
	// (network reordering) and assert the rendered path, swept every 10ms, is
	// MONOTONIC non-decreasing in x with no single-step jump larger than the
	// true per-frame delta (no teleport).
	std::printf("[b] jitter absorption: out-of-order arrival renders monotonic\n");
	{
		rem::SnapshotBuffer buf;
		// True path: x = 0.04 * t  (2.0 units per 50ms). Push in scrambled order.
		const uint64_t times[] = {0, 100, 50, 200, 150, 250};
		for (uint64_t t : times) {
			buf.push(rem::Snapshot{t, Vec3{0.04f * static_cast<float>(t), 0.0f, 0.0f}, 0.0f});
		}
		check("buffer sorted: 6 snapshots kept", buf.size() == 6);

		// Sweep the render cursor 0..250 in 10ms steps; x must be monotonic and
		// each 10ms step must move ~0.4 units (no teleport). Also every interior
		// sample must equal the true line 0.04*t (linear path → interp is exact).
		float prev_x = -1.0f;
		bool monotonic = true, no_teleport = true, matches_truth = true;
		for (uint64_t t = 0; t <= 250; t += 10) {
			auto s = buf.sample(t);
			if (s.position.x + 1e-4f < prev_x) monotonic = false;
			if (prev_x >= 0.0f && (s.position.x - prev_x) > 0.5f) no_teleport = false;
			if (!near(s.position.x, 0.04f * static_cast<float>(t), 1e-2f)) matches_truth = false;
			prev_x = s.position.x;
		}
		check("path monotonic non-decreasing", monotonic);
		check("no teleport (step <= true delta)", no_teleport);
		check("interpolated path matches true line", matches_truth);

		// A duplicate resend of t=100 with a CORRECTED x must replace, not dupe.
		buf.push(rem::Snapshot{100, Vec3{99.0f, 0.0f, 0.0f}, 0.0f});
		check("duplicate timestamp replaces (no size growth)", buf.size() == 6);
		check("corrected value wins at t=100", near(buf.sample(100).position.x, 99.0f));
	}

	// =======================================================================
	// (c) ENTER / LEAVE / GAP handling.
	// =======================================================================
	std::printf("[c] enter/leave/gap: lifecycle + extrapolation cap + freeze\n");
	{
		rem::RemoteInterpolator interp;

		// ENTER: tracked immediately, renders at the spawn position (held, since
		// the render cursor is before/at the only snapshot).
		interp.on_enter(7, Vec3{3.0f, 0.0f, 1.0f}, 0.0f, /*server_t=*/1000);
		check("entity tracked after enter", interp.is_tracked(7));
		check("tracked_count == 1", interp.tracked_count() == 1);

		// LEAVE: forgotten; sampling returns kEmpty.
		interp.on_leave(7);
		check("entity gone after leave", !interp.is_tracked(7));
		check("sample after leave is empty",
		      interp.sample_entity(7, 2000).kind == rem::SampleKind::kEmpty);
		check("tracked_count == 0", interp.tracked_count() == 0);

		// GAP: two snapshots then silence. Extrapolate briefly, then FREEZE past
		// the 250ms cap. Path x = 0.04*t from t=0..50 (velocity 2 per 50ms).
		rem::SnapshotBuffer buf;
		buf.push(rem::Snapshot{0,  Vec3{0.0f, 0.0f, 0.0f}, 0.0f});
		buf.push(rem::Snapshot{50, Vec3{2.0f, 0.0f, 0.0f}, 0.0f});

		// Just past newest: extrapolate along the last segment (2 per 50ms).
		auto ext = buf.sample(100);   // 50ms past newest → +2 more → x=4
		check("gap: extrapolated kind", ext.kind == rem::SampleKind::kExtrapolated);
		check("gap: extrapolated x == 4.0", near(ext.position.x, 4.0f));

		// At the cap boundary (50 + 250 = 300): still extrapolating.
		check("gap: at cap still extrapolated",
		      buf.sample(300).kind == rem::SampleKind::kExtrapolated);

		// Beyond the cap (350 = 300ms past newest > 250 cap): frozen at newest.
		auto frozen = buf.sample(400);
		check("gap: beyond cap held/frozen", frozen.kind == rem::SampleKind::kHeld);
		check("gap: frozen at newest x == 2.0", near(frozen.position.x, 2.0f));

		// Single snapshot cannot extrapolate (no velocity) → held immediately.
		rem::SnapshotBuffer one;
		one.push(rem::Snapshot{0, Vec3{5.0f, 0.0f, 0.0f}, 0.0f});
		check("single snapshot ahead → held",
		      one.sample(100).kind == rem::SampleKind::kHeld);
		check("single snapshot held at its value", near(one.sample(100).position.x, 5.0f));

		// Before-oldest hold: a multi-snapshot buffer sampled at exactly the oldest
		// timestamp interpolates (endpoint); a render cursor strictly before the
		// oldest holds at the oldest position (we cannot invent the earlier past).
		rem::SnapshotBuffer past;
		past.push(rem::Snapshot{100, Vec3{7.0f, 0.0f, 0.0f}, 0.0f});
		past.push(rem::Snapshot{200, Vec3{9.0f, 0.0f, 0.0f}, 0.0f});
		auto before = past.sample(50);   // before oldest (100)
		check("before oldest → held", before.kind == rem::SampleKind::kHeld);
		check("before oldest holds at oldest x == 7.0", near(before.position.x, 7.0f));
	}

	// =======================================================================
	// (d) CLOCK SYNC — convergence to a known offset under noise + outliers.
	// =======================================================================
	// Model: server clock runs +5000ms ahead of client (true offset = 5000).
	// One-way latency ~25ms each way (rtt ~50ms), symmetric. For a symmetric
	// path the estimator recovers offset exactly; we add ±small noise to the
	// one-way split and assert convergence within tolerance, then inject gross
	// outliers and assert they are rejected and do not move the estimate.
	std::printf("[d] clock-sync: converge to known offset, reject outliers\n");
	{
		rem::ClockEstimator est;
		const int64_t kTrueOffset = 5000;

		// Deterministic pseudo-noise (no RNG dependency): a small repeating pattern
		// of asymmetry added to the one-way up-latency (down-latency absorbs the
		// rest so rtt stays fixed). Feed 40 round-trips.
		const int64_t noise[] = {0, 3, -2, 1, -4, 2, -1, 5, -3, 4};
		int64_t client_clock = 100000;   // arbitrary client monotonic start
		for (int i = 0; i < 40; ++i) {
			const int64_t rtt = 50;
			const int64_t up  = 25 + noise[i % 10];       // client→server leg
			const int64_t t0  = client_clock;
			const int64_t ts  = t0 + up + kTrueOffset;    // server stamps on arrival
			const int64_t t1  = t0 + rtt;                 // client receives after full rtt
			est.add_round_trip(static_cast<uint64_t>(t0),
			                   static_cast<uint64_t>(ts),
			                   static_cast<uint64_t>(t1));
			client_clock += 100;   // next ping 100ms later
		}
		check("clock: has estimate", est.has_estimate());
		// The noise is the one-way asymmetry; the midpoint estimator's error is
		// bounded by ~max|noise| (a few ms). Assert within 10ms of true offset.
		check("clock: offset converged within 10ms",
		      near_i(est.offset_ms(), kTrueOffset, 10));
		check("clock: rtt ~ 50ms", near_i(est.rtt_ms(), 50, 5));

		// Now inject gross outliers (a stalled reply: +2000ms spike). They must be
		// REJECTED and must NOT move the converged estimate.
		const int64_t before = est.offset_ms();
		bool rejected_all = true;
		for (int i = 0; i < 5; ++i) {
			const int64_t t0 = client_clock;
			const int64_t ts = t0 + 25 + kTrueOffset + 2000;   // +2000 spike
			const int64_t t1 = t0 + 50;
			bool accepted = est.add_round_trip(static_cast<uint64_t>(t0),
			                                   static_cast<uint64_t>(ts),
			                                   static_cast<uint64_t>(t1));
			if (accepted) rejected_all = false;
			client_clock += 100;
		}
		check("clock: gross outliers rejected", rejected_all);
		check("clock: estimate unchanged by outliers",
		      near_i(est.offset_ms(), before, 2));

		// A legitimate step (clock re-sync / drift) within the gate DOES track:
		// feed the true offset back and confirm it stays locked.
		for (int i = 0; i < 5; ++i) {
			const int64_t t0 = client_clock;
			const int64_t ts = t0 + 25 + kTrueOffset;
			const int64_t t1 = t0 + 50;
			est.add_round_trip(static_cast<uint64_t>(t0),
			                   static_cast<uint64_t>(ts),
			                   static_cast<uint64_t>(t1));
			client_clock += 100;
		}
		check("clock: stays locked after recovery",
		      near_i(est.offset_ms(), kTrueOffset, 10));

		// client_to_server / server_to_client round-trip through the offset.
		const uint64_t cs = est.client_to_server(1000);
		check("clock: client_to_server applies offset",
		      near_i(static_cast<int64_t>(cs), 1000 + kTrueOffset, 10));
		check("clock: server_to_client is the inverse",
		      near_i(static_cast<int64_t>(est.server_to_client(cs)), 1000, 1));
	}

	// =======================================================================
	// (e) INTEGRATION — clock offset + interpolation together.
	// =======================================================================
	// With a real offset, the facade must map client render time onto the server
	// timeline correctly before sampling. Offset = 5000 (server ahead). Snapshots
	// at server t=10000 (x=0) and t=10100 (x=10). A frame at client_now such that
	// server_now = client_now + 5000, render cursor = server_now - 100. Choose
	// client_now = 5150 → server_now = 10150 → render = 10050 → midpoint x=5.
	std::printf("[e] integration: clock offset + interp deliver the midpoint\n");
	{
		rem::RemoteInterpolator interp;
		// Seed the clock with symmetric round-trips at offset 5000.
		int64_t cc = 0;
		for (int i = 0; i < 12; ++i) {
			interp.on_clock_sync(static_cast<uint64_t>(cc),
			                     static_cast<uint64_t>(cc + 25 + 5000),
			                     static_cast<uint64_t>(cc + 50));
			cc += 100;
		}
		check("integration: offset locked ~5000",
		      near_i(interp.clock().offset_ms(), 5000, 10));

		interp.on_enter(9, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, /*server_t=*/10000);
		interp.on_update(9, Vec3{10.0f, 0.0f, 0.0f}, 0.0f, /*server_t=*/10100);

		// render cursor for client_now=5150: (5150+5000) - 100 = 10050.
		check("integration: render cursor == 10050",
		      near_i(static_cast<int64_t>(interp.render_server_time(5150)), 10050, 10));
		auto s = interp.sample_entity(9, 5150);
		check("integration: interpolated", s.kind == rem::SampleKind::kInterpolated);
		check("integration: midpoint x ~ 5.0", near(s.position.x, 5.0f, 0.6f));
	}

	std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail == 0 ? 0 : 1;
}
