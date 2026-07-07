// Project Meridian — MeridianTelemetry GDExtension class (issue #168).
//
// The thin Godot binding over the engine-free telemetry log capture core
// (telemetry_log_capture.*). This is the client half of the D-29 telemetry
// triple's ERROR/CRITICAL log channel, exposed to GDScript so the Godot side can
// route its logs in: an autoload wires Godot's logger + the C++ modules' log
// output into `capture_log()`, and calls `poll()` off the game loop to flush +
// ship batches. Mirrors the engine-free-core + thin-wrapper pattern of
// MeridianMovementController over the movement controller core (Client SAD §9.2).
//
// The wrapper's ONLY jobs are (a) marshal Godot Variants ↔ the core's plain
// types, (b) own the wall-clock (the core is clock-injected/deterministic), and
// (c) own the transport instance (config-injected endpoint URL, #167). All
// policy — the ERROR/CRITICAL gate, opt-out, rate limit, batching, no-PII
// serialization — lives in the tested engine-free core, not here.

#ifndef MERIDIAN_TELEMETRY_H
#define MERIDIAN_TELEMETRY_H

#include "telemetry_log_capture.h"
#include "telemetry_transport.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

namespace meridian {

// GDScript-facing severity enum (mirrors telemetry::Severity). Bound so GDScript
// can call `capture_log(MeridianTelemetry.SEVERITY_ERROR, msg, logger)` without
// a magic number; also lets a Godot-logger bridge map Godot's error levels in.
class MeridianTelemetry : public godot::RefCounted {
	GDCLASS(MeridianTelemetry, godot::RefCounted)

public:
	// Matches telemetry::Severity ordinals exactly.
	enum LogSeverity {
		SEVERITY_TRACE    = 0,
		SEVERITY_DEBUG    = 1,
		SEVERITY_INFO     = 2,
		SEVERITY_WARN     = 3,
		SEVERITY_ERROR    = 4,
		SEVERITY_CRITICAL = 5,
	};

protected:
	static void _bind_methods();

public:
	MeridianTelemetry();
	~MeridianTelemetry();

	// ── Configuration (called once at boot by the autoload) ──────────────────
	// Session/build/platform context — the ONLY context attached (privacy §2a);
	// NO PII. session_id is an ephemeral opaque token (privacy §3).
	void configure(const godot::String &session_id, const godot::String &build,
	               const godot::String &platform);

	// Batching + rate-limit knobs (defaults match CaptureConfig).
	void set_batching(int batch_max_events, int batch_flush_interval_ms);
	void set_rate_limit(int max_events, int window_ms);

	// Opt-out toggle (privacy §5). When false, capture/ship nothing. The client
	// setting drives this; the SHIPPED default is the owner's judgment-call #2.
	void set_enabled(bool enabled);
	bool is_enabled() const;

	// Configurable shipping endpoint (#167 ingest, which may not exist yet). When
	// empty/unset, a NullTransport (no-op/local sink) is used — telemetry is never
	// a hard dependency (privacy §6). A non-empty URL installs the HTTP transport.
	void set_endpoint(const godot::String &url);
	godot::String get_endpoint() const;

	// ── Capture (called for every log line the sink sees) ────────────────────
	// Offer one log event. Returns true if captured, false if dropped (opted out,
	// below ERROR, or rate-limited). Cheap — safe to call on the logger thread.
	bool capture_log(int severity, const godot::String &message,
	                 const godot::String &logger);

	// ── Poll (called off the game loop, e.g. a low-frequency timer) ──────────
	// If a batch is due (N events or T ms), flush + ship it through the configured
	// transport. Returns true if a batch was shipped/attempted. This is where the
	// "never block the game loop" contract is honored: keep it off the frame path.
	bool poll();

	// Force an immediate flush + ship (e.g. on clean shutdown, to drain the tail).
	bool flush_now();

	// ── Introspection (diagnostics / GDScript tests) ─────────────────────────
	godot::Dictionary get_stats() const;   // {buffered, pending_dropped, captured, dropped, enabled, has_endpoint}

private:
	uint64_t now_ms() const;   // wall-clock epoch ms (owned here; core is injected)

	std::unique_ptr<telemetry::LogCapture> capture_;
	std::unique_ptr<telemetry::ITransport> transport_;
	godot::String endpoint_url_;
};

} // namespace meridian

VARIANT_ENUM_CAST(meridian::MeridianTelemetry::LogSeverity);

#endif // MERIDIAN_TELEMETRY_H
