// Project Meridian — engine-free unit test for the deterministic REPLAY HARNESS
// (issue #106). NO Godot: compiles against the plain-C++ cores (replay_harness.*,
// movement_controller.*) + the #101 headers only, so it runs in any C++17 toolchain
// without a Godot runtime (Client SAD §9.2). Plain-main style, mirroring the #102/
// #103 tests; ctest-wired via test/CMakeLists.txt.
//
// This is the DESYNC REGRESSION NET (Client SAD §9.2 / R5 "every desync bug is
// ours"). It pins:
//   1. RECORD -> REPLAY BIT-IDENTICAL — running the same recording twice yields
//      byte-for-byte identical per-tick traces (no hidden nondeterminism).
//   2. SERIALISE ROUND-TRIP — a recording/result/fixture serialises and parses back
//      to the EXACT same bits (fixtures are trustworthy on disk).
//   3. GOLDEN FIXTURES — each checked-in test/fixtures/*.replay parses, its recording
//      matches the in-source definition, and a FRESH replay reproduces the stored
//      golden bit-for-bit (+ the golden hash). This is the check-in regression net.
//   4. TAMPER DETECTION — flipping ONE input diverges the replay from the golden and
//      the net reports the exact tick (proves it actually catches desync).
//   5. REPEATED-REPLAY DETERMINISM — N replays of the same recording all agree.
//   6. MALFORMED INPUT — a truncated/garbled fixture is REJECTED, not mis-parsed.

#include "movement_constants.h"
#include "replay_fixture_defs.h"
#include "replay_harness.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rp = meridian::replay;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

// Locate the checked-in fixtures relative to THIS source file, so the test runs from
// any build dir (same convention as pack_manifest_core_test).
static std::string fixture_dir() {
	std::string f = __FILE__;
	const std::size_t slash = f.find_last_of("/\\");
	const std::string dir = (slash == std::string::npos) ? std::string(".") : f.substr(0, slash);
	return dir + "/fixtures";
}

static bool read_file(const std::string& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

int main() {
	std::printf("meridian replay harness test (#106)\n");

	// =======================================================================
	// 1. RECORD -> REPLAY BIT-IDENTICAL (in-memory).
	// =======================================================================
	// Running the same recording twice must produce byte-for-byte identical
	// traces. If ANY hidden nondeterminism existed (uninitialised state, float
	// order via a static, etc.) the two runs would diverge here.
	std::printf("[1] record -> replay bit-identical\n");
	{
		const rp::Recording rec = rp::fixtures::forward_run_small_correction();
		const rp::ReplayResult a = rp::run_recording(rec);
		const rp::ReplayResult b = rp::run_recording(rec);
		check("non-empty trace", !a.frames.empty());
		check("two runs are bit-identical", rp::bit_equal(a, b));
		check("first_divergent_frame == -1 for identical runs",
		      rp::first_divergent_frame(a, b) == -1);
		check("trace hashes equal", rp::trace_hash(a) == rp::trace_hash(b));
	}

	// =======================================================================
	// 2. SERIALISE ROUND-TRIP — recording / result / fixture parse back EXACTLY.
	// =======================================================================
	std::printf("[2] serialise round-trip is exact\n");
	{
		const rp::Recording rec = rp::fixtures::jump_arc_snap_correction();

		// Recording: serialise -> parse -> re-serialise must be byte-identical, and
		// replaying the parsed recording must match replaying the original.
		const std::string rec_txt = rp::serialize_recording(rec);
		rp::Recording rec2;
		check("recording parses", rp::parse_recording(rec_txt, rec2));
		check("recording re-serialises identically",
		      rp::serialize_recording(rec2) == rec_txt);
		check("parsed recording replays bit-identically",
		      rp::bit_equal(rp::run_recording(rec), rp::run_recording(rec2)));

		// Result: serialise -> parse -> compare bit-for-bit.
		const rp::ReplayResult res = rp::run_recording(rec);
		const std::string res_txt = rp::serialize_result(res);
		rp::ReplayResult res2;
		check("result parses", rp::parse_result(res_txt, res2));
		check("parsed result is bit-identical", rp::bit_equal(res, res2));
		check("result re-serialises identically", rp::serialize_result(res2) == res_txt);

		// Fixture: make -> serialise -> parse -> compare recording + golden + hash.
		const rp::Fixture fx = rp::make_fixture("jump_arc_snap_correction", rec);
		const std::string fx_txt = rp::serialize_fixture(fx);
		rp::Fixture fx2;
		check("fixture parses", rp::parse_fixture(fx_txt, fx2));
		check("fixture name preserved", fx2.name == fx.name);
		check("fixture golden hash preserved", fx2.golden_hash == fx.golden_hash);
		check("fixture recording round-trips",
		      rp::serialize_recording(fx2.recording) == rp::serialize_recording(fx.recording));
		check("fixture golden round-trips bit-identically",
		      rp::bit_equal(fx2.golden, fx.golden));
		check("golden hash matches its own trace", fx.golden_hash == rp::trace_hash(fx.golden));
	}

	// =======================================================================
	// 3. GOLDEN FIXTURES — each checked-in fixture replays to its recorded golden.
	// =======================================================================
	// THE regression net: load the on-disk fixture, confirm its recording is the one
	// we define in source, then replay it FRESH and assert the new trace is
	// bit-identical to the golden captured when the fixture was minted. A numeric
	// change anywhere in the sim breaks this.
	std::printf("[3] checked-in golden fixtures replay bit-identically\n");
	{
		for (const auto& def : rp::fixtures::all_fixtures()) {
			const std::string path = fixture_dir() + "/" + def.filename;
			std::string text;
			const bool loaded = read_file(path, text);
			check((def.filename + ": file present").c_str(), loaded);
			if (!loaded) continue;

			rp::Fixture fx;
			check((def.filename + ": parses").c_str(), rp::parse_fixture(text, fx));

			// The on-disk recording matches the in-source definition (fixture is not
			// stale relative to the generator input).
			check((def.filename + ": recording matches source def").c_str(),
			      rp::serialize_recording(fx.recording) == rp::serialize_recording(def.recording));

			// A fresh replay reproduces the stored golden — bit-for-bit and by hash.
			const rp::ReplayResult fresh = rp::run_recording(fx.recording);
			const long div = rp::first_divergent_frame(fresh, fx.golden);
			if (div != -1) {
				std::printf("        DIVERGENCE at frame %ld — sim drifted from golden!\n", div);
			}
			check((def.filename + ": fresh replay == golden (bit-identical)").c_str(),
			      rp::bit_equal(fresh, fx.golden));
			check((def.filename + ": fresh replay hash == golden hash").c_str(),
			      rp::trace_hash(fresh) == fx.golden_hash);
		}
	}

	// =======================================================================
	// 4. TAMPER DETECTION — flipping ONE input diverges from the golden.
	// =======================================================================
	// Proves the net actually catches desync: a single altered input must break
	// bit-identity, and first_divergent_frame must pinpoint the tick.
	std::printf("[4] tampering with one input is detected\n");
	{
		const rp::Recording rec = rp::fixtures::forward_run_small_correction();
		const rp::ReplayResult golden = rp::run_recording(rec);

		// Tamper: at tick 5, strafe right instead of running straight forward.
		rp::Recording tampered = rec;
		const std::size_t tick = 5;
		tampered.events[tick].input.move_x = 1.0f;   // was 0.0f
		const rp::ReplayResult drift = rp::run_recording(tampered);

		check("tampered replay is NOT bit-identical to golden",
		      !rp::bit_equal(drift, golden));
		check("tampered replay hash differs", rp::trace_hash(drift) != rp::trace_hash(golden));
		const long div = rp::first_divergent_frame(golden, drift);
		std::printf("        divergence first seen at frame %ld (tampered tick %zu)\n", div, tick);
		check("divergence reported at/after the tampered tick",
		      div >= 0 && static_cast<std::size_t>(div) >= tick);

		// A tampered CORRECTION is caught too (not just inputs).
		rp::Recording tampered_corr = rec;
		tampered_corr.events[8].correction.position.x += 0.01f;   // nudge the server x
		check("tampered correction is detected",
		      !rp::bit_equal(rp::run_recording(tampered_corr), golden));
	}

	// =======================================================================
	// 5. REPEATED-REPLAY DETERMINISM — N replays all agree.
	// =======================================================================
	std::printf("[5] repeated replays are all identical\n");
	{
		const rp::Recording rec = rp::fixtures::jump_arc_snap_correction();
		const rp::ReplayResult first = rp::run_recording(rec);
		const uint64_t h0 = rp::trace_hash(first);
		bool all_identical = true;
		for (int i = 0; i < 16; ++i) {
			const rp::ReplayResult r = rp::run_recording(rec);
			if (!rp::bit_equal(r, first) || rp::trace_hash(r) != h0) all_identical = false;
		}
		check("16 replays are all bit-identical to the first", all_identical);

		// The snap fixture must actually snap somewhere (the case has teeth).
		bool saw_snap = false;
		for (const auto& f : first.frames) if (f.snapped) saw_snap = true;
		check("snap fixture exercises the snap path", saw_snap);
	}

	// =======================================================================
	// 6. MALFORMED INPUT — a truncated/garbled fixture is REJECTED.
	// =======================================================================
	// A corrupt fixture must fail to parse, never silently yield a wrong recording.
	std::printf("[6] malformed fixtures are rejected\n");
	{
		rp::Recording junk;
		check("empty string rejected", !rp::parse_recording("", junk));
		check("wrong magic rejected", !rp::parse_recording("not-a-recording v1\n", junk));

		// Truncate a valid fixture mid-stream: header + magic present, body cut off.
		const rp::Fixture fx =
		    rp::make_fixture("t", rp::fixtures::forward_run_small_correction());
		const std::string full = rp::serialize_fixture(fx);
		const std::string truncated = full.substr(0, full.size() / 2);
		rp::Fixture bad;
		check("truncated fixture rejected", !rp::parse_fixture(truncated, bad));

		// A declared event count larger than the events present must fail.
		rp::Recording over;
		check("over-declared event count rejected",
		      !rp::parse_recording(
		          "meridian-replay-recording v1\nplane_y 00000000\n"
		          "start 00000000 00000000 00000000 00000000 00000000 00000000 1 0 00000000\n"
		          "events 5\n",
		          over));
	}

	std::printf(g_fail == 0 ? "\nALL REPLAY HARNESS TESTS PASSED\n"
	                        : "\n%d REPLAY HARNESS TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
