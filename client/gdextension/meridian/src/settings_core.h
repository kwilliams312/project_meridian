// Project Meridian — client SETTINGS store + first-run auto-benchmark CORE
// (issue #108).
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores"): this header
// and its .cpp contain NO Godot types. The GDExtension node (`MeridianSettings`,
// meridian_settings.*) is a thin wrapper that (a) reads/writes the settings text
// off disk via Godot's FileAccess at `user://settings.cfg`, and (b) marshals this
// core's typed values into GDScript. ALL policy — the typed schema + defaults, the
// INI serialization, corrupt-file tolerance, the benchmark→quality-tier mapping,
// and the first-run-once orchestration — lives here so it is unit-tested in a
// plain C++17 test with no Godot runtime, the same discipline as the #102 movement
// controller, #104 remote interpolator, #105 camera, and #107 pack-manifest cores.
//
// What this implements (Client SAD §4.3 "Settings & scalability persistence" +
// §2.1 boot flow "auto-benchmark on first run"; Client PRD §2.2 tiers):
//   * A typed, categorised settings model (graphics / audio / input / network),
//     each knob individually overridable, with sensible defaults — persisted to a
//     `user://` config file, versioned with a migration shim so preset schema
//     changes never reset a player's configs.
//   * A first-run auto-benchmark SKELETON: on first launch (no settings file) a
//     lightweight, BOUNDED probe runs, its result maps to a starting quality tier
//     (Low/Medium/High/Epic), the tier is persisted, and first-run is marked
//     complete so it never re-runs.
//
// ─────────────────────────────────────────────────────────────────────────────
// ⚠ BENCHMARK IS A SKELETON — see IBenchmark / CpuProbeBenchmark below.
// The default probe times a FIXED, bounded CPU workload as a documented PROXY for
// machine capability. It is NOT a GPU/render benchmark — the real crowded-scene
// frame-time benchmark (Client PRD §9) plugs in LATER by implementing IBenchmark
// and being handed to `apply_first_run_benchmark()`. The seam is the IBenchmark
// interface; nothing else in the store/flow changes when the real bench lands.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef MERIDIAN_SETTINGS_CORE_H
#define MERIDIAN_SETTINGS_CORE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace meridian::settings {

// ===========================================================================
// Quality tiers — the scalability presets (Client PRD §2.2). Ordinals are wire-
// and file-stable (persisted as `graphics.quality_tier`); never renumber.
// ===========================================================================
enum class QualityTier : int {
	Low    = 0,  // GTX 1060 6GB / M1 8GB floor — 30 FPS @ 1080p
	Medium = 1,  // GTX 1660 Super class        — 60 FPS @ 1080p
	High   = 2,  // RTX 3070 class              — 60 FPS @ 1440p, SDFGI on
	Epic   = 3,  // RTX 4070+ class             — 60+ FPS @ 4K/FSR2
};

// Human-readable tier name for logs / the settings UI ("low"/"medium"/...).
const char *tier_name(QualityTier tier);

// The schema version stamped into every settings file. Bump when the key set or a
// value's meaning changes, and add a step to migrate() (Client SAD §4.3 migration
// shim — a schema change must never silently reset a player's config).
constexpr int kSchemaVersion = 1;

// ===========================================================================
// Typed value — a small tagged union covering the four settings value types.
// Kept deliberately tiny (no std::variant dependency, C++17-portable) so the
// store is trivially serialisable and every get/set is type-checked.
// ===========================================================================
enum class ValueType : int { Bool = 0, Int = 1, Float = 2, String = 3 };

struct Value {
	ValueType type = ValueType::Int;
	bool        b = false;
	int64_t     i = 0;
	double      f = 0.0;
	std::string s;

	static Value of_bool(bool v)              { Value x; x.type = ValueType::Bool;   x.b = v; return x; }
	static Value of_int(int64_t v)            { Value x; x.type = ValueType::Int;    x.i = v; return x; }
	static Value of_float(double v)           { Value x; x.type = ValueType::Float;  x.f = v; return x; }
	static Value of_string(const std::string &v) { Value x; x.type = ValueType::String; x.s = v; return x; }

	bool operator==(const Value &o) const;
	bool operator!=(const Value &o) const { return !(*this == o); }
};

// One known setting: its category, key, type, default, and a doc string. The full
// schema (schema()) is the single source of truth for defaults + types, so a
// missing file yields defaults and an unknown/mismatched key can never corrupt the
// store.
struct SettingDef {
	const char *section;   // "graphics" | "audio" | "input" | "network"
	const char *key;       // e.g. "quality_tier"
	ValueType   type;
	Value       default_value;
	const char *doc;
};

// The frozen M0 settings taxonomy. Sections group into the [graphics]/[audio]/
// [input]/[network] categories of Client SAD §4.3 / PRD §2.2. Every knob is
// individually overridable (a preset is only a STARTING set of these values).
const std::vector<SettingDef> &schema();

// ===========================================================================
// Serialization outcome — how a load() interpreted the on-disk bytes.
// ===========================================================================
enum class LoadStatus : int {
	// The file existed and parsed cleanly (every recognised line applied). This is
	// the "existing settings loaded" case — first-run does NOT re-benchmark.
	Ok = 0,
	// No file / empty input — a fresh install. The store holds pure defaults; the
	// caller SHOULD run the first-run benchmark.
	Missing = 1,
	// The file existed but was unreadable as our format (no recognised keys, or a
	// binary/garbage blob). The store is reset to safe defaults; nothing throws.
	// The caller treats this like a fresh install (re-benchmark + rewrite), so a
	// corrupt config self-heals instead of bricking the client (Client SAD §5.2
	// "never a silent degrade").
	Corrupt = 2,
};

struct LoadResult {
	LoadStatus status = LoadStatus::Missing;
	int  file_schema_version = 0;   // schema_version read from the file (0 if none)
	int  applied_keys = 0;          // recognised key=value lines applied
	int  malformed_lines = 0;       // lines that looked like data but failed to parse
	bool migrated = false;          // a migration step ran (file_schema < kSchemaVersion)

	bool is_first_run() const { return status == LoadStatus::Missing || status == LoadStatus::Corrupt; }
};

// ===========================================================================
// SettingsStore — the typed, categorised settings model + INI (de)serialisation.
//
// Construction seeds every key from schema() defaults, so a store is ALWAYS
// complete and valid even before any file is read. Typed get/set validate the
// key's declared type: a mismatched get returns the supplied fallback (never
// throws, never coerces silently), and a mismatched set is rejected.
// ===========================================================================
class SettingsStore {
public:
	SettingsStore();  // seeded with schema() defaults

	// Reset every key back to its schema default (used on Corrupt, and by the
	// settings-UI "restore defaults" action).
	void reset_to_defaults();

	// --- Typed getters. `fallback` is returned when the key is unknown OR its
	//     declared type differs from the getter (defensive — never coerces). ---
	bool        get_bool(const std::string &section, const std::string &key, bool fallback = false) const;
	int64_t     get_int(const std::string &section, const std::string &key, int64_t fallback = 0) const;
	double      get_float(const std::string &section, const std::string &key, double fallback = 0.0) const;
	std::string get_string(const std::string &section, const std::string &key, const std::string &fallback = "") const;

	// --- Typed setters. Return false (no-op) if the key is unknown or the value's
	//     type does not match the key's declared type. ---
	bool set_bool(const std::string &section, const std::string &key, bool v);
	bool set_int(const std::string &section, const std::string &key, int64_t v);
	bool set_float(const std::string &section, const std::string &key, double v);
	bool set_string(const std::string &section, const std::string &key, const std::string &v);

	// True if `<section>.<key>` is a known setting in the schema.
	bool has(const std::string &section, const std::string &key) const;

	// --- Quality-tier convenience (graphics.quality_tier is stored as an int). ---
	QualityTier quality_tier() const;
	void set_quality_tier(QualityTier tier);

	// --- First-run bookkeeping (stored as network-agnostic meta keys). ---
	bool first_run_complete() const;
	void set_first_run_complete(bool done);

	// --- (De)serialisation ---------------------------------------------------
	// Serialise to the on-disk INI text (a leading `schema_version=` + a section
	// per category). Deterministic: keys emit in schema() order so the file is
	// stable across saves (clean diffs, reproducible).
	std::string serialize() const;

	// Parse INI text produced by serialize() (or hand-edited). Tolerant by design:
	//   * unknown [section]/key lines are ignored (forward-compat with newer files),
	//   * a value that fails to coerce to its key's type keeps the default and bumps
	//     malformed_lines,
	//   * `file_absent == true` (or empty text) → LoadStatus::Missing, pure defaults,
	//   * non-empty text with ZERO recognised keys → LoadStatus::Corrupt + defaults.
	// The store is reset to defaults first, so a partial file yields defaults for
	// everything it does not mention.
	LoadResult load(const std::string &text, bool file_absent);

	// Direct read-only view of every current (fully-qualified "section.key") value —
	// used by the wrapper to marshal the whole set, and by tests.
	const std::map<std::string, Value> &values() const { return values_; }

private:
	// Bring a just-parsed file up to kSchemaVersion (Client SAD §4.3 migration
	// shim). v0/absent → v1 is the identity today; the switch is the seam for real
	// per-version steps. Returns true if any step ran.
	bool migrate(int from_version);

	const SettingDef *find_def(const std::string &section, const std::string &key) const;

	// Fully-qualified "section.key" → current Value. Always complete (all schema
	// keys present) after construction / reset_to_defaults / load.
	std::map<std::string, Value> values_;
	bool first_run_complete_ = false;
};

// ===========================================================================
// Benchmark seam — the SKELETON's plug point (see the file header warning).
// ===========================================================================

// The result of ONE benchmark probe. `score` is a machine-capability proxy where
// HIGHER = more capable (so it maps monotonically onto the tiers). `elapsed_ms` is
// the wall time the probe took (bounded), and `method` documents WHICH probe ran
// so a persisted result is self-describing when the real GPU bench replaces this.
struct BenchmarkResult {
	double      score = 0.0;       // capability proxy; higher = faster machine
	double      elapsed_ms = 0.0;  // bounded wall time of the probe
	std::string method;            // e.g. "cpu-fixed-workload-skeleton-v1"
};

// The seam. The real crowded-scene GPU/frame-time benchmark (Client PRD §9)
// implements this and is handed to apply_first_run_benchmark() — the store and the
// first-run flow are unchanged. Kept minimal (one method) so a fake in tests is
// trivial and the real bench has room to do whatever it needs internally.
class IBenchmark {
public:
	virtual ~IBenchmark() = default;
	virtual BenchmarkResult run() = 0;
};

// The M0 default probe — a SKELETON. Times a FIXED, bounded arithmetic workload
// (no allocation, no I/O, no threads) with a steady clock and reports throughput
// (iterations/second) as the capability `score`. This is an intentionally crude
// PROXY: it correlates loosely with single-core CPU speed, NOT with GPU/render
// capability, and exists only so the first-run flow has a real, deterministic-
// shaped seam to exercise until the PRD §9 GPU benchmark lands. `iterations`
// bounds the run so it never blocks boot; the default is tuned to a few ms.
class CpuProbeBenchmark : public IBenchmark {
public:
	explicit CpuProbeBenchmark(uint64_t iterations = kDefaultIterations);
	BenchmarkResult run() override;

	static constexpr uint64_t kDefaultIterations = 2'000'000ULL;

private:
	uint64_t iterations_;
};

// Thresholds mapping a BenchmarkResult.score onto a QualityTier. Boundaries are
// INCLUSIVE lower bounds: score >= epic_min → Epic, else >= high_min → High, else
// >= medium_min → Medium, else Low. Defaults are DOCUMENTED PLACEHOLDERS for the
// CPU-proxy skeleton (they are not calibrated to real hardware — the real bench
// ships its own calibrated thresholds). Injectable so tests pin exact boundaries.
struct TierThresholds {
	double medium_min = 5.0e6;   // score >= this → at least Medium
	double high_min   = 2.0e7;   // score >= this → at least High
	double epic_min   = 8.0e7;   // score >= this → Epic
};

// Pure mapping (the tested heart of the benchmark→tier decision).
QualityTier tier_for_score(double score, const TierThresholds &t = TierThresholds{});

// ===========================================================================
// First-run orchestration — the "benchmark once, then never again" logic.
// ===========================================================================
struct FirstRunResult {
	bool            ran_benchmark = false;  // true only on a genuine first run
	QualityTier     selected_tier = QualityTier::Medium;
	BenchmarkResult benchmark;              // valid only when ran_benchmark
};

// The engine-free first-run decision. If `existing_settings_loaded` is false (a
// fresh install or a corrupt/self-healed file), run `bench` once, map its score to
// a tier, write that tier into `store`, and mark first-run complete — so a later
// launch (existing_settings_loaded == true) is a NO-OP and never re-benchmarks.
// The wrapper passes `!load_result.is_first_run()` as `existing_settings_loaded`.
FirstRunResult apply_first_run_benchmark(SettingsStore &store,
                                         bool existing_settings_loaded,
                                         IBenchmark &bench,
                                         const TierThresholds &thresholds = TierThresholds{});

}  // namespace meridian::settings

#endif  // MERIDIAN_SETTINGS_CORE_H
