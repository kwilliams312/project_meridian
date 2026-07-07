// Project Meridian — MeridianSettings GDExtension binding (issue #108).
// Thin Godot wrapper over the tested engine-free settings + first-run auto-
// benchmark core. All policy lives in the core; this file does user:// file I/O,
// marshals typed values to/from GDScript, and owns the benchmark seam. See
// meridian_settings.h.

#include "meridian_settings.h"

#include "settings_core.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

using namespace godot;

namespace meridian {

namespace {

// Default persisted location (Client SAD §4.3 "a user settings file, Godot user://").
constexpr const char *kDefaultConfigPath = "user://settings.cfg";

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

String from_std(const std::string &s) {
	return String::utf8(s.c_str());
}

int clamp_tier(int tier) {
	if (tier < static_cast<int>(settings::QualityTier::Low)) {
		return static_cast<int>(settings::QualityTier::Low);
	}
	if (tier > static_cast<int>(settings::QualityTier::Epic)) {
		return static_cast<int>(settings::QualityTier::Epic);
	}
	return tier;
}

} // namespace

MeridianSettings::MeridianSettings() : config_path_(kDefaultConfigPath) {}
MeridianSettings::~MeridianSettings() = default;

std::unique_ptr<settings::IBenchmark> MeridianSettings::make_benchmark() const {
	// SEAM: M0 default is the CPU-probe SKELETON (a documented CPU-timing proxy, NOT
	// a GPU benchmark). The real crowded-scene frame-time benchmark (Client PRD §9)
	// replaces this body — return the GPU bench (or select by platform) here and
	// nothing else in this class changes.
	return std::make_unique<settings::CpuProbeBenchmark>();
}

String MeridianSettings::read_config_text(bool &absent) const {
	absent = !FileAccess::file_exists(config_path_);
	if (absent) {
		return String();
	}
	Ref<FileAccess> f = FileAccess::open(config_path_, FileAccess::READ);
	if (f.is_null()) {
		// Exists but unreadable — treat as corrupt-ish: hand the core empty text but
		// report NOT absent, so load() judges it (a truly empty read is benign
		// Missing; the caller re-benchmarks either way). Log for diagnostics.
		UtilityFunctions::push_warning(
			String("MeridianSettings: could not open ") + config_path_ +
			" for read (error " +
			String::num_int64(static_cast<int64_t>(FileAccess::get_open_error())) + ")");
		return String();
	}
	const String text = f->get_as_text();
	f->close();
	return text;
}

Dictionary MeridianSettings::load_or_initialize() {
	bool absent = false;
	const String text = read_config_text(absent);

	const settings::LoadResult lr = store_.load(to_std(text), absent);

	// The core's first-run decision: a fresh install OR a corrupt/self-healed file
	// benchmarks; an existing, parseable file does not.
	const bool existing_loaded = !lr.is_first_run();
	std::unique_ptr<settings::IBenchmark> bench = make_benchmark();
	const settings::FirstRunResult fr =
		settings::apply_first_run_benchmark(store_, existing_loaded, *bench);

	// Persist when we just benchmarked (a fresh/self-healed config must be written
	// so the next launch skips the benchmark).
	bool saved = false;
	if (fr.ran_benchmark) {
		saved = save();
	}

	const char *status_str =
		(lr.status == settings::LoadStatus::Ok) ? "ok" :
		(lr.status == settings::LoadStatus::Corrupt) ? "corrupt" : "missing";
	if (lr.status == settings::LoadStatus::Corrupt) {
		UtilityFunctions::push_warning(
			String("MeridianSettings: ") + config_path_ +
			" was unreadable/corrupt — reset to defaults and re-ran first-run benchmark.");
	}

	Dictionary d;
	d["status"] = String(status_str);
	d["first_run"] = fr.ran_benchmark;
	d["ran_benchmark"] = fr.ran_benchmark;
	d["quality_tier"] = static_cast<int>(fr.selected_tier);
	d["quality_tier_name"] = String(settings::tier_name(fr.selected_tier));
	d["benchmark_score"] = fr.ran_benchmark ? fr.benchmark.score : 0.0;
	d["benchmark_ms"] = fr.ran_benchmark ? fr.benchmark.elapsed_ms : 0.0;
	d["benchmark_method"] = fr.ran_benchmark ? from_std(fr.benchmark.method) : String();
	d["saved"] = saved;
	d["path"] = config_path_;
	return d;
}

bool MeridianSettings::save() {
	Ref<FileAccess> f = FileAccess::open(config_path_, FileAccess::WRITE);
	if (f.is_null()) {
		UtilityFunctions::push_error(
			String("MeridianSettings: could not open ") + config_path_ +
			" for write (error " +
			String::num_int64(static_cast<int64_t>(FileAccess::get_open_error())) + ")");
		return false;
	}
	f->store_string(from_std(store_.serialize()));
	f->close();
	return true;
}

void MeridianSettings::set_config_path(const String &path) {
	config_path_ = path.is_empty() ? String(kDefaultConfigPath) : path;
}

String MeridianSettings::get_config_path() const {
	return config_path_;
}

bool MeridianSettings::get_bool(const String &section, const String &key, bool fallback) const {
	return store_.get_bool(to_std(section), to_std(key), fallback);
}

int64_t MeridianSettings::get_int(const String &section, const String &key, int64_t fallback) const {
	return store_.get_int(to_std(section), to_std(key), fallback);
}

double MeridianSettings::get_float(const String &section, const String &key, double fallback) const {
	return store_.get_float(to_std(section), to_std(key), fallback);
}

String MeridianSettings::get_string(const String &section, const String &key, const String &fallback) const {
	return from_std(store_.get_string(to_std(section), to_std(key), to_std(fallback)));
}

bool MeridianSettings::set_bool(const String &section, const String &key, bool v) {
	return store_.set_bool(to_std(section), to_std(key), v);
}

bool MeridianSettings::set_int(const String &section, const String &key, int64_t v) {
	return store_.set_int(to_std(section), to_std(key), v);
}

bool MeridianSettings::set_float(const String &section, const String &key, double v) {
	return store_.set_float(to_std(section), to_std(key), v);
}

bool MeridianSettings::set_string(const String &section, const String &key, const String &v) {
	return store_.set_string(to_std(section), to_std(key), to_std(v));
}

bool MeridianSettings::has(const String &section, const String &key) const {
	return store_.has(to_std(section), to_std(key));
}

int MeridianSettings::get_quality_tier() const {
	return static_cast<int>(store_.quality_tier());
}

void MeridianSettings::set_quality_tier(int tier) {
	store_.set_quality_tier(static_cast<settings::QualityTier>(clamp_tier(tier)));
}

String MeridianSettings::tier_name(int tier) const {
	return String(settings::tier_name(static_cast<settings::QualityTier>(clamp_tier(tier))));
}

bool MeridianSettings::is_first_run_complete() const {
	return store_.first_run_complete();
}

void MeridianSettings::reset_to_defaults() {
	store_.reset_to_defaults();
}

Dictionary MeridianSettings::as_dictionary() const {
	// {section: {key: value}} — driven by the schema so the whole taxonomy renders.
	Dictionary out;
	for (const settings::SettingDef &def : settings::schema()) {
		const String section = String(def.section);
		const String key = String(def.key);
		Dictionary sect = out.has(section) ? static_cast<Dictionary>(out[section]) : Dictionary();
		switch (def.type) {
			case settings::ValueType::Bool:
				sect[key] = store_.get_bool(def.section, def.key);
				break;
			case settings::ValueType::Int:
				sect[key] = static_cast<int64_t>(store_.get_int(def.section, def.key));
				break;
			case settings::ValueType::Float:
				sect[key] = store_.get_float(def.section, def.key);
				break;
			case settings::ValueType::String:
				sect[key] = from_std(store_.get_string(def.section, def.key));
				break;
		}
		out[section] = sect;
	}
	return out;
}

void MeridianSettings::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_or_initialize"), &MeridianSettings::load_or_initialize);
	ClassDB::bind_method(D_METHOD("save"), &MeridianSettings::save);

	ClassDB::bind_method(D_METHOD("set_config_path", "path"), &MeridianSettings::set_config_path);
	ClassDB::bind_method(D_METHOD("get_config_path"), &MeridianSettings::get_config_path);

	ClassDB::bind_method(D_METHOD("get_bool", "section", "key", "fallback"),
	                     &MeridianSettings::get_bool, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_int", "section", "key", "fallback"),
	                     &MeridianSettings::get_int, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_float", "section", "key", "fallback"),
	                     &MeridianSettings::get_float, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_string", "section", "key", "fallback"),
	                     &MeridianSettings::get_string, DEFVAL(String()));

	ClassDB::bind_method(D_METHOD("set_bool", "section", "key", "value"), &MeridianSettings::set_bool);
	ClassDB::bind_method(D_METHOD("set_int", "section", "key", "value"), &MeridianSettings::set_int);
	ClassDB::bind_method(D_METHOD("set_float", "section", "key", "value"), &MeridianSettings::set_float);
	ClassDB::bind_method(D_METHOD("set_string", "section", "key", "value"), &MeridianSettings::set_string);

	ClassDB::bind_method(D_METHOD("has", "section", "key"), &MeridianSettings::has);

	ClassDB::bind_method(D_METHOD("get_quality_tier"), &MeridianSettings::get_quality_tier);
	ClassDB::bind_method(D_METHOD("set_quality_tier", "tier"), &MeridianSettings::set_quality_tier);
	ClassDB::bind_method(D_METHOD("tier_name", "tier"), &MeridianSettings::tier_name);
	ClassDB::bind_method(D_METHOD("is_first_run_complete"), &MeridianSettings::is_first_run_complete);
	ClassDB::bind_method(D_METHOD("reset_to_defaults"), &MeridianSettings::reset_to_defaults);
	ClassDB::bind_method(D_METHOD("as_dictionary"), &MeridianSettings::as_dictionary);

	BIND_ENUM_CONSTANT(TIER_LOW);
	BIND_ENUM_CONSTANT(TIER_MEDIUM);
	BIND_ENUM_CONSTANT(TIER_HIGH);
	BIND_ENUM_CONSTANT(TIER_EPIC);
}

} // namespace meridian
