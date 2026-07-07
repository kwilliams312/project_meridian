// Project Meridian — MeridianPackMount GDExtension binding (issue #107).
// Thin Godot wrapper over the tested engine-free IF-5 pack-manifest verify core.
// All policy lives in the core; this file reads the manifest bytes off disk,
// marshals the core's result into a GDScript Dictionary, and caches the resolved
// content identity for later IF-2 realm validation. See meridian_pack_mount.h.

#include "meridian_pack_mount.h"

#include "pack_manifest_core.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

using namespace godot;

namespace meridian {

namespace {

// Convert a Godot String (UTF-8) to std::string for the engine-free core.
std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

// Join a pack directory and the fixed manifest filename, tolerating a trailing
// slash. Kept simple — the pack dir is a client-config / boot path, not user text.
String manifest_path_for(const String &pack_dir) {
	String d = pack_dir;
	if (d.ends_with("/")) {
		d = d.substr(0, d.length() - 1);
	}
	return d + "/pack.manifest.json";
}

} // namespace

MeridianPackMount::MeridianPackMount() = default;
MeridianPackMount::~MeridianPackMount() = default;

void MeridianPackMount::set_expected_godot_version(const String &version) {
	expected_godot_version_ = version;
}

String MeridianPackMount::get_expected_godot_version() const {
	return expected_godot_version_;
}

void MeridianPackMount::set_expected_content_hash(const String &hash) {
	expected_content_hash_ = hash;
}

String MeridianPackMount::get_expected_content_hash() const {
	return expected_content_hash_;
}

pack::PackVerifyOptions MeridianPackMount::make_options() const {
	pack::PackVerifyOptions opts;
	opts.expected_godot_version = to_std(expected_godot_version_);
	const String h = expected_content_hash_.strip_edges();
	if (!h.is_empty()) {
		opts.expected_content_hash = to_std(h);
	}
	return opts;
}

Dictionary MeridianPackMount::report_to_dict(const pack::PackReport &rep) {
	Dictionary d;
	d["verdict"] = static_cast<int>(rep.verdict);
	d["ok"] = (rep.verdict == pack::PackVerdict::kOk);
	d["hard_fail"] = rep.hard_fail;
	d["reason"] = String::utf8(rep.reason.c_str());
	d["content_hash"] = String::utf8(rep.content_hash.c_str());
	d["content_version"] = String::utf8(rep.content_version.c_str());
	d["godot_version"] = String::utf8(rep.godot_version.c_str());
	d["content_schema_version"] = static_cast<int>(rep.content_schema_version);
	d["entry_count"] = static_cast<int64_t>(rep.entry_count);

	// Cache the resolved identity on success so the boot flow + the IF-2 connect
	// can read it without re-verifying. A hard fail clears the mounted state.
	if (rep.verdict == pack::PackVerdict::kOk) {
		mounted_ = true;
		content_hash_ = String::utf8(rep.content_hash.c_str());
		content_version_ = String::utf8(rep.content_version.c_str());
		content_schema_version_ = static_cast<int>(rep.content_schema_version);
	} else {
		mounted_ = false;
	}
	return d;
}

Dictionary MeridianPackMount::mount_and_verify(const String &pack_dir) {
	const String path = manifest_path_for(pack_dir);

	// Read the manifest bytes via Godot's FileAccess (res:// / user:// aware). A
	// missing / unreadable file is a hard-fail malformed report — the client
	// cannot trust content it cannot even read (SAD §5.2). We do NOT throw across
	// the GDExtension boundary; the failure is a verdict the boot scene branches on.
	if (!FileAccess::file_exists(path)) {
		pack::PackReport rep;
		rep.verdict = pack::PackVerdict::kMalformed;
		rep.hard_fail = true;
		rep.reason = "pack.manifest.json not found at " + to_std(path) +
		             " — pack missing or not downloaded";
		return report_to_dict(rep);
	}

	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		pack::PackReport rep;
		rep.verdict = pack::PackVerdict::kMalformed;
		rep.hard_fail = true;
		rep.reason = "could not open pack.manifest.json at " + to_std(path) +
		             " (error " + std::to_string(static_cast<int>(FileAccess::get_open_error())) +
		             ")";
		return report_to_dict(rep);
	}
	const String json = f->get_as_text();
	f->close();

	const pack::PackReport rep = pack::verify_pack_manifest_json(to_std(json), make_options());
	if (rep.verdict != pack::PackVerdict::kOk) {
		UtilityFunctions::push_error(
			String("MeridianPackMount: pack verify FAILED [") +
			String(pack::pack_verdict_name(rep.verdict)) + "] — " +
			String::utf8(rep.reason.c_str()));
	}
	return report_to_dict(rep);
}

Dictionary MeridianPackMount::verify_manifest_json(const String &manifest_json) {
	const pack::PackReport rep =
		pack::verify_pack_manifest_json(to_std(manifest_json), make_options());
	if (rep.verdict != pack::PackVerdict::kOk) {
		UtilityFunctions::push_error(
			String("MeridianPackMount: pack verify FAILED [") +
			String(pack::pack_verdict_name(rep.verdict)) + "] — " +
			String::utf8(rep.reason.c_str()));
	}
	return report_to_dict(rep);
}

bool MeridianPackMount::is_mounted() const {
	return mounted_;
}

String MeridianPackMount::get_content_hash() const {
	return content_hash_;
}

String MeridianPackMount::get_content_version() const {
	return content_version_;
}

int MeridianPackMount::get_content_schema_version() const {
	return content_schema_version_;
}

String MeridianPackMount::verdict_name(int verdict) {
	return String(pack::pack_verdict_name(static_cast<pack::PackVerdict>(verdict)));
}

void MeridianPackMount::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_expected_godot_version", "version"),
	                     &MeridianPackMount::set_expected_godot_version);
	ClassDB::bind_method(D_METHOD("get_expected_godot_version"),
	                     &MeridianPackMount::get_expected_godot_version);
	ClassDB::bind_method(D_METHOD("set_expected_content_hash", "hash"),
	                     &MeridianPackMount::set_expected_content_hash);
	ClassDB::bind_method(D_METHOD("get_expected_content_hash"),
	                     &MeridianPackMount::get_expected_content_hash);

	ClassDB::bind_method(D_METHOD("mount_and_verify", "pack_dir"),
	                     &MeridianPackMount::mount_and_verify);
	ClassDB::bind_method(D_METHOD("verify_manifest_json", "manifest_json"),
	                     &MeridianPackMount::verify_manifest_json);

	ClassDB::bind_method(D_METHOD("is_mounted"), &MeridianPackMount::is_mounted);
	ClassDB::bind_method(D_METHOD("get_content_hash"), &MeridianPackMount::get_content_hash);
	ClassDB::bind_method(D_METHOD("get_content_version"),
	                     &MeridianPackMount::get_content_version);
	ClassDB::bind_method(D_METHOD("get_content_schema_version"),
	                     &MeridianPackMount::get_content_schema_version);

	ClassDB::bind_static_method("MeridianPackMount", D_METHOD("verdict_name", "verdict"),
	                            &MeridianPackMount::verdict_name);

	BIND_ENUM_CONSTANT(VERDICT_OK);
	BIND_ENUM_CONSTANT(VERDICT_MALFORMED);
	BIND_ENUM_CONSTANT(VERDICT_SCHEMA_MISMATCH);
	BIND_ENUM_CONSTANT(VERDICT_ENGINE_MISMATCH);
	BIND_ENUM_CONSTANT(VERDICT_HASH_MISMATCH);
}

} // namespace meridian
