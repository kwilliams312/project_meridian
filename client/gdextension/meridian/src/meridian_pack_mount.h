// Project Meridian — MeridianPackMount GDExtension class (issue #107).
//
// The thin Godot binding over the engine-free IF-5 pack-manifest verify core
// (pack_manifest_core.*). This is the client counterpart to worldd's #89 world-DB
// boot check: the Boot scene (Client SAD §2.3 flow `Boot → mount → manifest ok,
// content hash H`) hands this class a pack directory, it reads + verifies the
// pack's `pack.manifest.json`, and exposes the verdict + resolved content
// identity (the content hash the client holds for IF-2 realm validation) so the
// boot flow can PROCEED (kOk) or HALT with a clear message (any hard-fail verdict
// — SAD §5.2 "clear rejection message plus updater hand-off, never a silent
// degrade"). Mirrors the engine-free-core + thin-wrapper pattern of
// MeridianTelemetry over the telemetry core (Client SAD §9.2).
//
// The wrapper's ONLY jobs are (a) read the manifest bytes off disk via Godot's
// FileAccess (res:// or user:// aware), (b) marshal the core's plain result into
// a GDScript Dictionary, and (c) hold the mounted content identity for later
// realm validation. ALL policy — the schema/engine/hash checks, the fail-fast
// verdicts, the well-formedness rules — lives in the tested engine-free core.
//
// M0 SCOPE (honest, per #121): the Godot-native `.pck` mount
// (ProjectSettings::load_resource_pack) is a follow-up once the pinned engine's
// import pipeline lands. At M0 this class verifies the MANIFEST (the testable
// IF-5 contract, the #107 deliverable). `mount_and_verify()` reads the manifest
// from the given pack directory; the actual load_resource_pack call is wired in
// with the follow-up (a one-line addition once the .pck exists), gated behind the
// same kOk verdict this class already computes.

#ifndef MERIDIAN_PACK_MOUNT_H
#define MERIDIAN_PACK_MOUNT_H

#include "pack_manifest_core.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace meridian {

// GDScript-facing verdict enum (mirrors pack::PackVerdict ordinals). Bound so the
// Boot scene can branch on `result.verdict == MeridianPackMount.VERDICT_OK`
// without a magic number, and log the specific failure class.
class MeridianPackMount : public godot::RefCounted {
	GDCLASS(MeridianPackMount, godot::RefCounted)

public:
	// Matches pack::PackVerdict ordinals exactly.
	enum MountVerdict {
		VERDICT_OK              = 0,
		VERDICT_MALFORMED       = 1,
		VERDICT_SCHEMA_MISMATCH = 2,
		VERDICT_ENGINE_MISMATCH = 3,
		VERDICT_HASH_MISMATCH   = 4,
	};

protected:
	static void _bind_methods();

public:
	MeridianPackMount();
	~MeridianPackMount();

	// ── Optional verify pins (set once at boot before mount_and_verify) ───────
	// The client's PINNED Godot engine version (client/ENGINE_VERSION). When set
	// non-empty, the pack's godot_version MUST match or the mount is rejected as
	// VERDICT_ENGINE_MISMATCH (PRD R8; SAD §5.2 "engine pin"). Empty (default) ->
	// the engine pin is not checked (the M0 default until the boot scene wires the
	// real pin in from ENGINE_VERSION).
	void set_expected_godot_version(const godot::String &version);
	godot::String get_expected_godot_version() const;

	// The realm/operator-pinned "this is the compile we deployed" content hash
	// (SAD §5.2 "the realm accepts only on content-hash match"). When set to a
	// 64-hex string, the pack's content_hash MUST equal it or the mount is
	// rejected as VERDICT_HASH_MISMATCH. Empty (default) -> no pin, the hash tie is
	// not checked (a well-formed, schema/engine-matching manifest is accepted).
	void set_expected_content_hash(const godot::String &hash);
	godot::String get_expected_content_hash() const;

	// ── Mount + verify (the Boot-scene entry point) ───────────────────────────
	// Given a pack directory (e.g. "res://meridian/core" or a "user://packs/…"
	// path), read `<pack_dir>/pack.manifest.json` and verify it against this
	// client's contract + any pins set above. Returns a Dictionary the Boot scene
	// consumes:
	//   {
	//     "verdict":        int (MountVerdict),
	//     "ok":             bool  (true only when verdict == VERDICT_OK),
	//     "hard_fail":      bool  (true unless verdict == VERDICT_OK),
	//     "reason":         String (one-line explanation for logs / boot UX),
	//     "content_hash":   String (the pack content hash — held for IF-2 realm
	//                               validation; empty on a malformed manifest),
	//     "content_version":String ("<namespace>@<version>" the client mounted),
	//     "godot_version":  String (the pack's pinned engine version),
	//     "content_schema_version": int,
	//     "entry_count":    int,
	//   }
	// On a successful verify the resolved identity is ALSO cached on this instance
	// (get_content_hash / get_content_version / is_mounted) so the boot flow and
	// the later IF-2 connect can read it without re-verifying. A hard-fail leaves
	// is_mounted() false.
	godot::Dictionary mount_and_verify(const godot::String &pack_dir);

	// Verify a manifest whose JSON bytes are already in hand (e.g. from a mounted
	// .pck, or a test). Same return Dictionary + same caching as mount_and_verify.
	godot::Dictionary verify_manifest_json(const godot::String &manifest_json);

	// ── Resolved identity (read after a successful mount_and_verify) ──────────
	bool          is_mounted() const;            // last verify was VERDICT_OK
	godot::String get_content_hash() const;      // for IF-2 realm content-hash match
	godot::String get_content_version() const;   // "<namespace>@<version>"
	int           get_content_schema_version() const;

	// Human-readable verdict name (logs / diagnostics; e.g. "schema-mismatch").
	static godot::String verdict_name(int verdict);

private:
	// Build the return Dictionary from a core report + cache the identity on kOk.
	godot::Dictionary report_to_dict(const pack::PackReport &rep);

	// Assemble the current verify options from the pins set on this instance.
	pack::PackVerifyOptions make_options() const;

	godot::String expected_godot_version_;
	godot::String expected_content_hash_;

	// Cached resolved identity from the last successful verify.
	bool          mounted_ = false;
	godot::String content_hash_;
	godot::String content_version_;
	int           content_schema_version_ = 0;
};

} // namespace meridian

VARIANT_ENUM_CAST(meridian::MeridianPackMount::MountVerdict);

#endif // MERIDIAN_PACK_MOUNT_H
