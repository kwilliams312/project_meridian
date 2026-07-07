// Project Meridian — MeridianCrashReporter GDExtension class (issue #109).
//
// The thin Godot binding over the engine-free crash cores (crash_handler.*,
// crash_report_core.*, crash_upload_queue.*). The client half of the D-29
// telemetry triple's CRASH channel (Client SAD §5.5). Exposed to GDScript so an
// autoload can, at boot:
//   1. install()  — arm the fatal-signal handler with the no-PII context + the
//      resolved user:// crash directory, BEFORE the net/worker threads start, and
//   2. flush_pending() — ship any crash report left by a PREVIOUS run through the
//      SAME #167/#168 transport (GodotHttpTransport), then delete the delivered
//      files. Call it once shortly after boot (off the first frame).
//
// Mirrors the MeridianTelemetry wrapper: all policy (report format, envelope,
// upload sweep, async-safe capture) lives in the tested engine-free cores; this
// wrapper only marshals Godot types, owns the config-injected transport, and
// keeps the client-side crash counter (the "client-side equivalent" of the
// server's meridian_client_crash_total, #297).

#ifndef MERIDIAN_CRASH_REPORTER_H
#define MERIDIAN_CRASH_REPORTER_H

#include "telemetry_transport.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>

namespace meridian {

class MeridianCrashReporter : public godot::RefCounted {
	GDCLASS(MeridianCrashReporter, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	MeridianCrashReporter();
	~MeridianCrashReporter();

	// ── Install (called once at boot, before worker threads start) ───────────
	// Arms the fatal-signal handler. `crash_dir` must be an ABSOLUTE OS path (the
	// autoload resolves `user://crash` via ProjectSettings.globalize_path first).
	// The context is the no-PII session/build/platform triple (privacy §3).
	// Returns true if a real handler was installed (false on a platform without
	// the POSIX path — the Crashpad seam; capture is then inert but the object is
	// still usable to flush reports left by another build).
	bool install(const godot::String &session_id, const godot::String &build,
	             const godot::String &platform, const godot::String &crash_dir);

	// True if a real crash handler is currently active on this platform.
	bool is_active() const;

	// ── Endpoint (the #167 ingest; may not exist yet) ────────────────────────
	// Empty/unset ⇒ NullTransport (reports are kept on disk, never lost). A
	// non-empty URL installs the SAME GodotHttpTransport the #168 log channel uses.
	void set_endpoint(const godot::String &url);
	godot::String get_endpoint() const;

	// ── Upload (called shortly after boot, off the first frame) ──────────────
	// Sweep the crash directory: ship every pending report from a previous run
	// through the transport, delete each delivered file, and bump the client-side
	// crash counter. Returns the number of reports SHIPPED this sweep. Safe to
	// call repeatedly (a no-op when the queue is empty).
	int flush_pending();

	// Number of crash report files currently queued on disk.
	int pending_count() const;

	// ── Introspection (diagnostics / GDScript tests) ─────────────────────────
	// {active, has_endpoint, pending, crash_shipped_total, crash_found_total,
	//  crash_dir}. crash_shipped_total is the client-side crash counter (#297).
	godot::Dictionary get_stats() const;

private:
	std::unique_ptr<telemetry::ITransport> transport_;
	godot::String endpoint_url_;
	godot::String crash_dir_;
	bool          installed_ = false;

	// Client-side crash counters (the "client-side equivalent" of the server's
	// meridian_client_crash_total, #297). Lifetime totals across flush sweeps.
	uint64_t crash_shipped_total_ = 0;
	uint64_t crash_found_total_   = 0;
};

} // namespace meridian

#endif // MERIDIAN_CRASH_REPORTER_H
