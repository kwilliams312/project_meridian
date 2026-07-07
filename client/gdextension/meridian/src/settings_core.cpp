// Project Meridian — client settings store + first-run auto-benchmark CORE
// implementation (issue #108). Engine-free C++17 — see settings_core.h.

#include "settings_core.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace meridian::settings {

namespace {

// Meta keys (persisted at the top of the file, outside the category sections).
constexpr const char *kMetaSection = "meta";
constexpr const char *kFirstRunKey = "first_run_complete";

std::string qualify(const std::string &section, const std::string &key) {
	return section + "." + key;
}

// Trim ASCII whitespace from both ends (no locale, no allocation surprises).
std::string trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
	return s.substr(a, b - a);
}

// Parse a bool token ("true"/"false"/"1"/"0"). Returns false via `ok` on anything
// else so a malformed value keeps the default instead of silently reading false.
bool parse_bool(const std::string &tok, bool &ok) {
	if (tok == "true" || tok == "1")  { ok = true; return true; }
	if (tok == "false" || tok == "0") { ok = true; return false; }
	ok = false;
	return false;
}

int64_t parse_int(const std::string &tok, bool &ok) {
	if (tok.empty()) { ok = false; return 0; }
	errno = 0;
	char *end = nullptr;
	const long long v = std::strtoll(tok.c_str(), &end, 10);
	ok = (errno == 0 && end != nullptr && *end == '\0');
	return static_cast<int64_t>(v);
}

double parse_float(const std::string &tok, bool &ok) {
	if (tok.empty()) { ok = false; return 0.0; }
	errno = 0;
	char *end = nullptr;
	const double v = std::strtod(tok.c_str(), &end);
	ok = (errno == 0 && end != nullptr && *end == '\0' && std::isfinite(v));
	return v;
}

// Serialise a value to its file token. Floats emit a fixed 6-dp form so the file
// is stable across saves (no locale-dependent shortest-round-trip drift).
std::string value_token(const Value &v) {
	switch (v.type) {
		case ValueType::Bool:   return v.b ? "true" : "false";
		case ValueType::Int:    return std::to_string(v.i);
		case ValueType::Float: {
			std::ostringstream os;
			os.precision(6);
			os << std::fixed << v.f;
			return os.str();
		}
		case ValueType::String: return v.s;
	}
	return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// Value / schema
// ---------------------------------------------------------------------------

bool Value::operator==(const Value &o) const {
	if (type != o.type) return false;
	switch (type) {
		case ValueType::Bool:   return b == o.b;
		case ValueType::Int:    return i == o.i;
		case ValueType::Float:  return std::fabs(f - o.f) <= 1e-9;
		case ValueType::String: return s == o.s;
	}
	return false;
}

const char *tier_name(QualityTier tier) {
	switch (tier) {
		case QualityTier::Low:    return "low";
		case QualityTier::Medium: return "medium";
		case QualityTier::High:   return "high";
		case QualityTier::Epic:   return "epic";
	}
	return "unknown";
}

const std::vector<SettingDef> &schema() {
	// Frozen M0 taxonomy (Client SAD §4.3 / PRD §2.2). Every knob is individually
	// overridable; a preset is only a STARTING assignment of these. Defaults are
	// conservative (Medium tier, native scale) so a machine that skips the bench
	// still gets a sane, playable config.
	static const std::vector<SettingDef> kSchema = {
		// ── [graphics] — the scalability knobs behind the §2.2 presets ──────────
		{"graphics", "quality_tier",   ValueType::Int,   Value::of_int(static_cast<int64_t>(QualityTier::Medium)),
			"scalability preset: 0=Low 1=Medium 2=High 3=Epic (PRD §2.2)"},
		{"graphics", "render_scale",   ValueType::Float, Value::of_float(1.0),
			"FSR2/MetalFX render scale, 0.5..1.0 (Low may drop below native, §2.1)"},
		{"graphics", "fullscreen",     ValueType::Bool,  Value::of_bool(true),
			"exclusive/borderless fullscreen vs windowed"},
		{"graphics", "vsync",          ValueType::Bool,  Value::of_bool(true),
			"vertical sync on the swapchain"},
		{"graphics", "shadow_cascades",ValueType::Int,   Value::of_int(3),
			"directional CSM cascade count (Low 2 / Med 3 / High 4, §2.1)"},
		{"graphics", "sdfgi",          ValueType::Bool,  Value::of_bool(false),
			"SDFGI dynamic GI (High/Epic on; Low/Med baked, §2.1)"},

		// ── [audio] — the M1 settings-UI bus sliders (PRD §2.6) ─────────────────
		{"audio", "master_volume", ValueType::Float, Value::of_float(1.0),  "master bus gain 0..1"},
		{"audio", "music_volume",  ValueType::Float, Value::of_float(0.8),  "music bus gain 0..1"},
		{"audio", "sfx_volume",    ValueType::Float, Value::of_float(1.0),  "SFX bus gain 0..1"},
		{"audio", "ambient_volume",ValueType::Float, Value::of_float(0.8),  "ambient bus gain 0..1"},

		// ── [input] — camera/mouse feel (mirrors #105 camera tunables) ──────────
		{"input", "mouse_sensitivity", ValueType::Float, Value::of_float(1.0),  "mouse-look sensitivity multiplier"},
		{"input", "invert_y",          ValueType::Bool,  Value::of_bool(false), "invert vertical mouse-look"},

		// ── [network] — client-side interp/prediction knobs (SAD §2.2/§5.1) ─────
		{"network", "interpolation_delay_ms", ValueType::Int,  Value::of_int(100),
			"remote-entity interpolation delay window, ms (#104)"},
		{"network", "prediction_enabled",     ValueType::Bool, Value::of_bool(true),
			"client-side input prediction (#102/#103)"},
	};
	return kSchema;
}

// ---------------------------------------------------------------------------
// SettingsStore
// ---------------------------------------------------------------------------

SettingsStore::SettingsStore() {
	reset_to_defaults();
}

void SettingsStore::reset_to_defaults() {
	values_.clear();
	for (const SettingDef &d : schema()) {
		values_[qualify(d.section, d.key)] = d.default_value;
	}
	first_run_complete_ = false;
}

const SettingDef *SettingsStore::find_def(const std::string &section, const std::string &key) const {
	for (const SettingDef &d : schema()) {
		if (section == d.section && key == d.key) return &d;
	}
	return nullptr;
}

bool SettingsStore::has(const std::string &section, const std::string &key) const {
	return find_def(section, key) != nullptr;
}

bool SettingsStore::get_bool(const std::string &section, const std::string &key, bool fallback) const {
	auto it = values_.find(qualify(section, key));
	if (it == values_.end() || it->second.type != ValueType::Bool) return fallback;
	return it->second.b;
}

int64_t SettingsStore::get_int(const std::string &section, const std::string &key, int64_t fallback) const {
	auto it = values_.find(qualify(section, key));
	if (it == values_.end() || it->second.type != ValueType::Int) return fallback;
	return it->second.i;
}

double SettingsStore::get_float(const std::string &section, const std::string &key, double fallback) const {
	auto it = values_.find(qualify(section, key));
	if (it == values_.end() || it->second.type != ValueType::Float) return fallback;
	return it->second.f;
}

std::string SettingsStore::get_string(const std::string &section, const std::string &key,
                                      const std::string &fallback) const {
	auto it = values_.find(qualify(section, key));
	if (it == values_.end() || it->second.type != ValueType::String) return fallback;
	return it->second.s;
}

bool SettingsStore::set_bool(const std::string &section, const std::string &key, bool v) {
	const SettingDef *d = find_def(section, key);
	if (!d || d->type != ValueType::Bool) return false;
	values_[qualify(section, key)] = Value::of_bool(v);
	return true;
}

bool SettingsStore::set_int(const std::string &section, const std::string &key, int64_t v) {
	const SettingDef *d = find_def(section, key);
	if (!d || d->type != ValueType::Int) return false;
	values_[qualify(section, key)] = Value::of_int(v);
	return true;
}

bool SettingsStore::set_float(const std::string &section, const std::string &key, double v) {
	const SettingDef *d = find_def(section, key);
	if (!d || d->type != ValueType::Float) return false;
	values_[qualify(section, key)] = Value::of_float(v);
	return true;
}

bool SettingsStore::set_string(const std::string &section, const std::string &key, const std::string &v) {
	const SettingDef *d = find_def(section, key);
	if (!d || d->type != ValueType::String) return false;
	values_[qualify(section, key)] = Value::of_string(v);
	return true;
}

QualityTier SettingsStore::quality_tier() const {
	const int64_t raw = get_int("graphics", "quality_tier",
	                            static_cast<int64_t>(QualityTier::Medium));
	if (raw < static_cast<int64_t>(QualityTier::Low) ||
	    raw > static_cast<int64_t>(QualityTier::Epic)) {
		return QualityTier::Medium;  // out-of-range file value → safe default
	}
	return static_cast<QualityTier>(raw);
}

void SettingsStore::set_quality_tier(QualityTier tier) {
	set_int("graphics", "quality_tier", static_cast<int64_t>(tier));
}

bool SettingsStore::first_run_complete() const {
	return first_run_complete_;
}

void SettingsStore::set_first_run_complete(bool done) {
	first_run_complete_ = done;
}

std::string SettingsStore::serialize() const {
	std::ostringstream os;
	os << "# Project Meridian — client settings (issue #108). Auto-generated; safe\n";
	os << "# to hand-edit. Unknown keys are ignored; a corrupt file self-heals to\n";
	os << "# defaults on next launch.\n";
	os << "schema_version=" << kSchemaVersion << "\n";
	os << "[" << kMetaSection << "]\n";
	os << kFirstRunKey << "=" << (first_run_complete_ ? "true" : "false") << "\n";

	// Emit category sections in schema() order for a stable, diff-friendly file.
	std::string current_section;
	for (const SettingDef &d : schema()) {
		if (d.section != current_section) {
			current_section = d.section;
			os << "[" << current_section << "]\n";
		}
		auto it = values_.find(qualify(d.section, d.key));
		const Value &v = (it != values_.end()) ? it->second : d.default_value;
		os << d.key << "=" << value_token(v) << "\n";
	}
	return os.str();
}

bool SettingsStore::migrate(int from_version) {
	// Client SAD §4.3 migration shim. v0 (absent) and v1 are identical today, so
	// this is the identity migration — the switch is the seam where a real per-
	// version transform lands (e.g. renaming/retyping a key) WITHOUT resetting the
	// player's other configs. Returns true if any step ran.
	bool ran = false;
	int v = from_version;
	// while (v < kSchemaVersion) { switch (v) { case 1: ...; break; } ++v; ran = true; }
	(void)v;
	return ran;
}

LoadResult SettingsStore::load(const std::string &text, bool file_absent) {
	// Always start from a complete, valid default set so any key the file omits (or
	// mangles) is left at its default rather than undefined.
	reset_to_defaults();

	LoadResult r;
	if (file_absent) {
		r.status = LoadStatus::Missing;
		return r;
	}

	// Empty (or whitespace-only) content is treated as a fresh install, not a
	// corruption — an empty file is a benign "nothing saved yet".
	if (trim(text).empty()) {
		r.status = LoadStatus::Missing;
		return r;
	}

	std::string section;             // current [section]
	int recognized_data_lines = 0;   // lines shaped like key=value (recognised OR not)
	std::istringstream is(text);
	std::string line;
	while (std::getline(is, line)) {
		const std::string t = trim(line);
		if (t.empty() || t[0] == '#' || t[0] == ';') continue;  // blank / comment

		if (t.front() == '[' && t.back() == ']') {
			section = trim(t.substr(1, t.size() - 2));
			continue;
		}

		const size_t eq = t.find('=');
		if (eq == std::string::npos) {
			// A non-blank, non-section, non-comment line with no '=' is junk. Count
			// it toward the malformed tally (drives the Corrupt verdict for garbage).
			++r.malformed_lines;
			continue;
		}
		++recognized_data_lines;
		const std::string key = trim(t.substr(0, eq));
		const std::string val = trim(t.substr(eq + 1));

		// Top-level (pre-section) meta: schema_version + first_run_complete.
		if (section.empty() || section == kMetaSection) {
			if (key == "schema_version") {
				bool ok = false;
				const int64_t sv = parse_int(val, ok);
				if (ok) r.file_schema_version = static_cast<int>(sv);
				else ++r.malformed_lines;
				continue;
			}
			if (key == kFirstRunKey) {
				bool ok = false;
				const bool fr = parse_bool(val, ok);
				if (ok) first_run_complete_ = fr;
				else ++r.malformed_lines;
				continue;
			}
			// Unknown meta key — ignore (forward-compat).
			continue;
		}

		const SettingDef *d = find_def(section, key);
		if (!d) continue;  // unknown [section]/key — ignore (forward-compat)

		bool ok = false;
		Value nv;
		switch (d->type) {
			case ValueType::Bool:   nv = Value::of_bool(parse_bool(val, ok)); break;
			case ValueType::Int:    nv = Value::of_int(parse_int(val, ok)); break;
			case ValueType::Float:  nv = Value::of_float(parse_float(val, ok)); break;
			case ValueType::String: nv = Value::of_string(val); ok = true; break;
		}
		if (!ok) { ++r.malformed_lines; continue; }  // bad value → keep default
		values_[qualify(section, key)] = nv;
		++r.applied_keys;
	}

	// Verdict: if the input had content but we recognised NO data lines at all
	// (only junk / no key=value), treat it as a corrupt blob and self-heal to
	// defaults. Otherwise it parsed (even a partial file counts as Ok).
	if (recognized_data_lines == 0) {
		reset_to_defaults();
		r.status = LoadStatus::Corrupt;
		return r;
	}

	r.migrated = migrate(r.file_schema_version);
	r.status = LoadStatus::Ok;
	return r;
}

// ---------------------------------------------------------------------------
// Benchmark skeleton + tier mapping + first-run orchestration
// ---------------------------------------------------------------------------

CpuProbeBenchmark::CpuProbeBenchmark(uint64_t iterations) : iterations_(iterations) {}

BenchmarkResult CpuProbeBenchmark::run() {
	// SKELETON PROXY (see settings_core.h header warning): a fixed, bounded,
	// allocation-free integer/float mix. `acc` is volatile-consumed so the compiler
	// cannot optimise the loop away, giving a stable timing signal. This is a crude
	// single-core CPU-speed proxy, NOT a GPU/render benchmark — it exists purely so
	// the first-run flow has a real seam to exercise until the PRD §9 GPU bench
	// implements IBenchmark and replaces this.
	const auto t0 = std::chrono::steady_clock::now();

	double acc = 1.0;
	uint64_t mix = 0x9E3779B97F4A7C15ULL;  // golden-ratio odd constant
	for (uint64_t k = 0; k < iterations_; ++k) {
		mix ^= (mix << 13);
		mix ^= (mix >> 7);
		mix ^= (mix << 17);
		acc += std::sqrt(static_cast<double>(mix & 0xFFFFULL) + 1.0);
	}
	const auto t1 = std::chrono::steady_clock::now();

	// Keep `acc` observable so the loop is not dead-code-eliminated.
	volatile double sink = acc;
	(void)sink;

	const double elapsed_ms =
		std::chrono::duration<double, std::milli>(t1 - t0).count();
	// Throughput (iterations/second) as the capability proxy; guard the div so a
	// sub-microsecond clock never yields inf.
	const double elapsed_s = elapsed_ms / 1000.0;
	const double score = (elapsed_s > 1e-9)
		? static_cast<double>(iterations_) / elapsed_s
		: 0.0;

	BenchmarkResult res;
	res.score = score;
	res.elapsed_ms = elapsed_ms;
	res.method = "cpu-fixed-workload-skeleton-v1";
	return res;
}

QualityTier tier_for_score(double score, const TierThresholds &t) {
	if (score >= t.epic_min)   return QualityTier::Epic;
	if (score >= t.high_min)   return QualityTier::High;
	if (score >= t.medium_min) return QualityTier::Medium;
	return QualityTier::Low;
}

FirstRunResult apply_first_run_benchmark(SettingsStore &store,
                                         bool existing_settings_loaded,
                                         IBenchmark &bench,
                                         const TierThresholds &thresholds) {
	FirstRunResult out;

	// Existing config already on disk → the first-run benchmark NEVER re-runs. This
	// is the "benchmark once" guarantee: the tier the player (or a prior run) chose
	// is authoritative.
	if (existing_settings_loaded) {
		out.ran_benchmark = false;
		out.selected_tier = store.quality_tier();
		return out;
	}

	// Genuine first run (fresh install or a corrupt/self-healed file): probe once,
	// map to a starting tier, persist it, and stamp first-run complete.
	out.benchmark = bench.run();
	out.selected_tier = tier_for_score(out.benchmark.score, thresholds);
	out.ran_benchmark = true;

	store.set_quality_tier(out.selected_tier);
	store.set_first_run_complete(true);
	return out;
}

}  // namespace meridian::settings
