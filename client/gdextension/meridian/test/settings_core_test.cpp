// Project Meridian — engine-free unit test for the client settings store +
// first-run auto-benchmark core (issue #108). NO Godot: it compiles against the
// plain-C++ core (settings_core.*) only, so it runs in any C++17 toolchain without
// a Godot runtime (Client SAD §9.2 engine-agnostic cores). Plain-main style,
// mirroring the #102 movement_controller_test / #105 tps_camera_core_test,
// ctest-wired via client/gdextension/meridian/test/CMakeLists.txt.
//
// Proves the settings + benchmark logic the MeridianSettings wrapper relies on:
//   1. ROUND-TRIP    — set values, serialize→load into a fresh store, values equal.
//   2. DEFAULTS      — a missing file yields schema defaults + first-run.
//   3. CORRUPT       — a garbage blob self-heals to defaults, flagged Corrupt, no crash.
//   4. PARTIAL       — a file mentioning some keys leaves the rest at defaults.
//   5. TYPED GET/SET — type mismatch on get/set is rejected, never coerces.
//   6. TIER MAPPING  — benchmark score → Low/Medium/High/Epic at the thresholds.
//   7. FIRST-RUN ONCE— first run benchmarks + picks a tier; a second launch does NOT.
//   8. CPU PROBE     — the skeleton probe is bounded and returns a positive score.

#include "settings_core.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace st = meridian::settings;

static int g_fail = 0;
static void check(const char *name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

static bool near(double a, double b, double eps = 1e-6) {
	return std::fabs(a - b) <= eps;
}

// A deterministic fake benchmark — returns a fixed score and counts how many times
// run() was called, so the "benchmark once" guarantee is asserted exactly.
class FakeBenchmark : public st::IBenchmark {
public:
	explicit FakeBenchmark(double score) : score_(score) {}
	st::BenchmarkResult run() override {
		++calls;
		st::BenchmarkResult r;
		r.score = score_;
		r.elapsed_ms = 1.0;
		r.method = "fake";
		return r;
	}
	int calls = 0;
private:
	double score_;
};

int main() {
	std::printf("meridian settings core test (#108)\n");

	// =======================================================================
	// 1. ROUND-TRIP: mutate across every category, serialize, load into a fresh
	//    store, and confirm every value survives byte-for-byte.
	// =======================================================================
	std::printf("[1] round-trip: save -> load preserves all values\n");
	{
		st::SettingsStore a;
		check("set graphics.quality_tier", a.set_int("graphics", "quality_tier",
		      static_cast<int64_t>(st::QualityTier::High)));
		check("set graphics.render_scale", a.set_float("graphics", "render_scale", 0.75));
		check("set graphics.fullscreen",   a.set_bool("graphics", "fullscreen", false));
		check("set audio.master_volume",   a.set_float("audio", "master_volume", 0.5));
		check("set input.invert_y",        a.set_bool("input", "invert_y", true));
		check("set network.interpolation_delay_ms",
		      a.set_int("network", "interpolation_delay_ms", 150));
		a.set_first_run_complete(true);

		const std::string text = a.serialize();

		st::SettingsStore b;
		const st::LoadResult r = b.load(text, /*file_absent=*/false);
		check("load status Ok", r.status == st::LoadStatus::Ok);
		check("no malformed lines", r.malformed_lines == 0);
		check("not first run after load", !r.is_first_run());

		check("quality_tier survived", b.quality_tier() == st::QualityTier::High);
		check("render_scale survived", near(b.get_float("graphics", "render_scale"), 0.75));
		check("fullscreen survived", b.get_bool("graphics", "fullscreen") == false);
		check("master_volume survived", near(b.get_float("audio", "master_volume"), 0.5));
		check("invert_y survived", b.get_bool("input", "invert_y") == true);
		check("interp_delay survived", b.get_int("network", "interpolation_delay_ms") == 150);
		check("first_run_complete survived", b.first_run_complete() == true);
	}

	// =======================================================================
	// 2. DEFAULTS ON MISSING FILE: a fresh install (no file) → schema defaults
	//    and a first-run verdict.
	// =======================================================================
	std::printf("[2] defaults: missing file -> schema defaults + first-run\n");
	{
		st::SettingsStore s;
		const st::LoadResult r = s.load("", /*file_absent=*/true);
		check("status Missing", r.status == st::LoadStatus::Missing);
		check("is_first_run", r.is_first_run());
		check("default tier is Medium", s.quality_tier() == st::QualityTier::Medium);
		check("default render_scale 1.0", near(s.get_float("graphics", "render_scale"), 1.0));
		check("default vsync true", s.get_bool("graphics", "vsync") == true);
		check("default prediction_enabled true", s.get_bool("network", "prediction_enabled") == true);
		check("first_run_complete false by default", s.first_run_complete() == false);
	}

	// =======================================================================
	// 3. CORRUPT FILE: a binary/garbage blob must not crash; it self-heals to
	//    defaults and reports Corrupt (which the flow treats as a first run).
	// =======================================================================
	std::printf("[3] corrupt: garbage -> defaults, flagged Corrupt, no crash\n");
	{
		st::SettingsStore s;
		// Pre-dirty the store to prove load() resets it.
		s.set_int("graphics", "quality_tier", static_cast<int64_t>(st::QualityTier::Epic));

		// Explicit length — the blob embeds NUL/high bytes and has NO `key=value`
		// line, so it must be judged Corrupt (a std::string from a bare C-literal
		// would truncate at the first NUL, so construct it from a sized buffer).
		const char raw[] = {'\x01', '\xFF', ' ', 'r', 'a', 'w', ' ', '\xDE', '\xAD',
		                    ' ', 'n', 'o', 't', ' ', 'o', 'u', 'r', ' ', 'f', 'o',
		                    'r', 'm', 'a', 't', '\n', '\x00', '\x7F', 'j', 'u', 'n', 'k'};
		const std::string garbage(raw, sizeof(raw));
		const st::LoadResult r = s.load(garbage, /*file_absent=*/false);
		check("status Corrupt", r.status == st::LoadStatus::Corrupt);
		check("is_first_run on corrupt", r.is_first_run());
		check("reset to default tier", s.quality_tier() == st::QualityTier::Medium);
		check("reset render_scale", near(s.get_float("graphics", "render_scale"), 1.0));
	}

	// =======================================================================
	// 4. PARTIAL FILE: unknown keys/sections ignored; a bad value keeps its
	//    default (malformed counted); untouched keys stay at defaults.
	// =======================================================================
	std::printf("[4] partial: recognised keys apply, rest default, unknowns ignored\n");
	{
		const std::string text =
			"schema_version=1\n"
			"[graphics]\n"
			"quality_tier=2\n"           // -> High (recognised)
			"render_scale=notanumber\n"  // -> malformed, keeps default 1.0
			"[unknown_section]\n"
			"whatever=123\n"             // -> ignored (unknown section)
			"[audio]\n"
			"bogus_key=1\n"              // -> ignored (unknown key)
			"master_volume=0.25\n";      // -> recognised
		st::SettingsStore s;
		const st::LoadResult r = s.load(text, /*file_absent=*/false);
		check("status Ok (had recognised keys)", r.status == st::LoadStatus::Ok);
		check("applied at least the two good keys", r.applied_keys >= 2);
		check("one malformed value counted", r.malformed_lines >= 1);
		check("quality_tier applied -> High", s.quality_tier() == st::QualityTier::High);
		check("bad render_scale kept default 1.0", near(s.get_float("graphics", "render_scale"), 1.0));
		check("master_volume applied", near(s.get_float("audio", "master_volume"), 0.25));
		check("untouched sfx_volume at default", near(s.get_float("audio", "sfx_volume"), 1.0));
	}

	// =======================================================================
	// 5. TYPED GET/SET: a get with the wrong type returns the fallback; a set
	//    with the wrong type (or unknown key) is rejected and changes nothing.
	// =======================================================================
	std::printf("[5] typed get/set: mismatches rejected, never coerce\n");
	{
		st::SettingsStore s;
		// quality_tier is Int — get_bool on it must return the fallback, not coerce.
		check("get_bool on int key returns fallback",
		      s.get_bool("graphics", "quality_tier", true) == true);
		check("get_float on int key returns fallback",
		      near(s.get_float("graphics", "quality_tier", 42.0), 42.0));
		// set_bool on an Int key must be rejected (no-op).
		check("set_bool on int key rejected", s.set_bool("graphics", "quality_tier", true) == false);
		check("int value unchanged after rejected set",
		      s.get_int("graphics", "quality_tier") == static_cast<int64_t>(st::QualityTier::Medium));
		// Unknown key rejected for both get + set.
		check("get on unknown key returns fallback",
		      s.get_int("graphics", "nonexistent", 7) == 7);
		check("set on unknown key rejected", s.set_int("graphics", "nonexistent", 1) == false);
		check("has() false for unknown", s.has("graphics", "nonexistent") == false);
		check("has() true for known", s.has("audio", "music_volume") == true);
		// A correct-type set succeeds.
		check("correct-type set succeeds", s.set_float("audio", "music_volume", 0.33));
		check("correct-type value applied", near(s.get_float("audio", "music_volume"), 0.33));
	}

	// =======================================================================
	// 6. BENCHMARK -> TIER MAPPING: inclusive lower-bound thresholds map score to
	//    Low / Medium / High / Epic at exactly the documented boundaries.
	// =======================================================================
	std::printf("[6] tier mapping: score -> Low/Medium/High/Epic thresholds\n");
	{
		st::TierThresholds t;  // medium_min=5e6, high_min=2e7, epic_min=8e7
		check("below medium -> Low",      st::tier_for_score(t.medium_min - 1.0, t) == st::QualityTier::Low);
		check("at medium_min -> Medium",  st::tier_for_score(t.medium_min, t) == st::QualityTier::Medium);
		check("between med/high -> Medium",st::tier_for_score((t.medium_min + t.high_min) / 2.0, t) == st::QualityTier::Medium);
		check("at high_min -> High",      st::tier_for_score(t.high_min, t) == st::QualityTier::High);
		check("between high/epic -> High",st::tier_for_score((t.high_min + t.epic_min) / 2.0, t) == st::QualityTier::High);
		check("at epic_min -> Epic",      st::tier_for_score(t.epic_min, t) == st::QualityTier::Epic);
		check("well above epic -> Epic",  st::tier_for_score(t.epic_min * 10.0, t) == st::QualityTier::Epic);
		check("zero score -> Low",        st::tier_for_score(0.0, t) == st::QualityTier::Low);
	}

	// =======================================================================
	// 7. FIRST-RUN ONCE: a genuine first run benchmarks + persists a tier; a
	//    subsequent launch (existing settings loaded) NEVER re-benchmarks.
	// =======================================================================
	std::printf("[7] first-run once: benchmark on install, skip on relaunch\n");
	{
		// Pick a score that lands in High for the default thresholds.
		FakeBenchmark bench(2.5e7);
		st::TierThresholds t;

		// --- Launch #1: fresh install (existing_settings_loaded == false). ---
		st::SettingsStore s;
		s.load("", /*file_absent=*/true);  // Missing -> first run
		const st::FirstRunResult r1 =
			st::apply_first_run_benchmark(s, /*existing_settings_loaded=*/false, bench, t);
		check("launch1 ran benchmark", r1.ran_benchmark == true);
		check("launch1 called bench exactly once", bench.calls == 1);
		check("launch1 selected High", r1.selected_tier == st::QualityTier::High);
		check("launch1 persisted tier into store", s.quality_tier() == st::QualityTier::High);
		check("launch1 marked first-run complete", s.first_run_complete() == true);

		// Persist + reload (simulate the file the wrapper would have written).
		const std::string saved = s.serialize();

		// --- Launch #2: existing file present (existing_settings_loaded == true). ---
		st::SettingsStore s2;
		const st::LoadResult lr = s2.load(saved, /*file_absent=*/false);
		check("launch2 loaded existing (not first run)", !lr.is_first_run());
		const st::FirstRunResult r2 =
			st::apply_first_run_benchmark(s2, /*existing_settings_loaded=*/!lr.is_first_run(), bench, t);
		check("launch2 did NOT run benchmark", r2.ran_benchmark == false);
		check("launch2 left bench call count at 1", bench.calls == 1);
		check("launch2 kept the persisted tier", s2.quality_tier() == st::QualityTier::High);
	}

	// =======================================================================
	// 8. CPU PROBE SKELETON: the default probe is bounded (completes) and yields a
	//    positive score + elapsed time + a self-describing method tag.
	// =======================================================================
	std::printf("[8] cpu probe: bounded, positive score, self-describing\n");
	{
		st::CpuProbeBenchmark probe(50000);  // small, fast bound for the test
		const st::BenchmarkResult res = probe.run();
		check("score positive", res.score > 0.0);
		check("elapsed non-negative", res.elapsed_ms >= 0.0);
		check("method tag is the skeleton", res.method == "cpu-fixed-workload-skeleton-v1");
		// The probe drives a real tier decision through the pure mapping.
		const st::QualityTier tier = st::tier_for_score(res.score);
		check("probe score maps to a valid tier",
		      tier == st::QualityTier::Low || tier == st::QualityTier::Medium ||
		      tier == st::QualityTier::High || tier == st::QualityTier::Epic);
	}

	std::printf(g_fail == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
