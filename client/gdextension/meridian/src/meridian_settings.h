// Project Meridian — MeridianSettings GDExtension class (issue #108).
//
// The thin Godot binding over the engine-free settings + first-run auto-benchmark
// core (settings_core.*). This is the `Settings` autoload's engine backing (Client
// SAD §2.1 "Autoload singletons (Net, Sim, Datastore, EventBus, Settings) wrap the
// GDExtension modules"; §4.3 settings persistence): the Boot flow calls
// `load_or_initialize()` once at startup, which reads `user://settings.cfg`, and —
// on a first launch (no file) or a corrupt file — runs the first-run auto-benchmark
// SKELETON, maps its result to a starting quality tier, and writes the file so it
// never re-benchmarks (SAD §2.1 "auto-benchmark on first run").
//
// Mirrors the engine-free-core + thin-wrapper pattern of MeridianPackMount over the
// pack core and MeridianTelemetry over the telemetry core (Client SAD §9.2). The
// wrapper's ONLY jobs are (a) user:// file I/O via Godot's FileAccess, (b) marshal
// the core's typed values to/from GDScript, and (c) own the benchmark seam. ALL
// policy — the schema/defaults, serialisation, corrupt tolerance, the tier mapping,
// and the first-run-once decision — lives in the tested engine-free core.
//
// ⚠ BENCHMARK SEAM (skeleton → real): load_or_initialize() drives the core's
// CpuProbeBenchmark by default — a documented CPU-timing PROXY, NOT a GPU/render
// benchmark. The real crowded-scene frame-time benchmark (Client PRD §9) plugs in
// by implementing settings::IBenchmark and being selected in `make_benchmark()`
// (the single seam method below); nothing else in this class changes.

#ifndef MERIDIAN_SETTINGS_H
#define MERIDIAN_SETTINGS_H

#include "settings_core.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

namespace meridian {

// GDScript-facing quality tiers (mirror settings::QualityTier ordinals). Bound so
// the settings UI can branch on `MeridianSettings.TIER_HIGH` without a magic number.
class MeridianSettings : public godot::RefCounted {
	GDCLASS(MeridianSettings, godot::RefCounted)

public:
	enum Tier {
		TIER_LOW    = 0,
		TIER_MEDIUM = 1,
		TIER_HIGH   = 2,
		TIER_EPIC   = 3,
	};

protected:
	static void _bind_methods();

public:
	MeridianSettings();
	~MeridianSettings();

	// ── Boot entry point ──────────────────────────────────────────────────────
	// Read `user://settings.cfg` (path overridable via set_config_path for tests /
	// tools). Behaviour:
	//   * file present + parses          → load it; NO benchmark (respects the
	//                                       player's saved tier).
	//   * file absent (fresh install)    → FIRST RUN: run the benchmark skeleton,
	//                                       map to a starting tier, save the file.
	//   * file present but corrupt       → self-heal: reset to defaults, RE-RUN the
	//                                       first-run benchmark, save (SAD §5.2
	//                                       "never a silent degrade").
	// Returns a Dictionary the Boot scene / Settings autoload consumes:
	//   {
	//     "status":            String ("ok" | "missing" | "corrupt"),
	//     "first_run":         bool   (true when the benchmark ran this launch),
	//     "ran_benchmark":     bool   (== first_run),
	//     "quality_tier":      int    (Tier),
	//     "quality_tier_name": String ("low"/"medium"/"high"/"epic"),
	//     "benchmark_score":   float  (proxy score; 0 when no benchmark ran),
	//     "benchmark_ms":      float  (probe wall time; 0 when none),
	//     "benchmark_method":  String (which probe ran; "" when none),
	//     "saved":             bool   (a file was (re)written this launch),
	//     "path":              String (the resolved config path),
	//   }
	godot::Dictionary load_or_initialize();

	// Persist the current settings to the config path. Returns true on success.
	bool save();

	// ── Config path (defaults to user://settings.cfg) ────────────────────────
	void set_config_path(const godot::String &path);
	godot::String get_config_path() const;

	// ── Typed accessors (section, key) — reject unknown keys / type mismatch ──
	bool          get_bool(const godot::String &section, const godot::String &key, bool fallback = false) const;
	int64_t       get_int(const godot::String &section, const godot::String &key, int64_t fallback = 0) const;
	double        get_float(const godot::String &section, const godot::String &key, double fallback = 0.0) const;
	godot::String get_string(const godot::String &section, const godot::String &key, const godot::String &fallback = godot::String()) const;

	bool set_bool(const godot::String &section, const godot::String &key, bool v);
	bool set_int(const godot::String &section, const godot::String &key, int64_t v);
	bool set_float(const godot::String &section, const godot::String &key, double v);
	bool set_string(const godot::String &section, const godot::String &key, const godot::String &v);

	bool has(const godot::String &section, const godot::String &key) const;

	// ── Quality tier + first-run bookkeeping ─────────────────────────────────
	int  get_quality_tier() const;            // Tier ordinal
	void set_quality_tier(int tier);          // clamps to a valid Tier
	godot::String tier_name(int tier) const;  // "low"/"medium"/"high"/"epic"
	bool is_first_run_complete() const;

	// Restore every knob to its schema default (settings-UI "restore defaults").
	void reset_to_defaults();

	// Every current value as a nested Dictionary {section: {key: value}} — for a
	// data-driven settings UI that renders the whole taxonomy.
	godot::Dictionary as_dictionary() const;

private:
	// THE benchmark seam. Returns the IBenchmark the first run drives. M0 default is
	// the CPU-probe SKELETON; the real PRD §9 GPU benchmark replaces the body here
	// (or selects between them) with no other change to this class.
	std::unique_ptr<settings::IBenchmark> make_benchmark() const;

	// Read the config file text via FileAccess. Sets `absent` true when the file
	// does not exist; returns "" in that case.
	godot::String read_config_text(bool &absent) const;

	settings::SettingsStore store_;
	godot::String config_path_;
};

} // namespace meridian

VARIANT_ENUM_CAST(meridian::MeridianSettings::Tier);

#endif // MERIDIAN_SETTINGS_H
