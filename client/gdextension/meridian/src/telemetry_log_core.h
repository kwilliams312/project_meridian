// Project Meridian — engine-free client telemetry log capture core (issue #168).
//
// The CLIENT half of the D-29 telemetry "triple" (docs/01-SYNC-DECISIONS.md §9
// rule 2; docs/telemetry-privacy.md §2a): capture ERROR/CRITICAL client log
// events, attach session/build/platform context (NO PII), batch + rate-limit
// them, serialize to the Sentry-compatible JSON envelope, and hand each batch to
// a transport. This header is the ENGINE-FREE core — plain C++17, NO Godot types
// — so it links into the client GDExtension, the headless bot, AND the plain
// doctest suite exactly like the movement controller core (Client SAD §9.2
// "engine-agnostic C++ cores … the bot client and unit tests link without
// Godot"). The thin GDExtension binding lives in meridian_telemetry.* (#168).
//
// ─────────────────────────────────────────────────────────────────────────
//   THE CONTRACT (docs/telemetry-privacy.md — the policy this code enforces)
// ─────────────────────────────────────────────────────────────────────────
// The client's entire outbound telemetry surface is EXACTLY three things
// (privacy §2a / D-29 rule 2). This module owns ONE of them — the ERROR/CRITICAL
// log channel. Hard rules this core encodes and the #168 tests assert:
//   • CAPTURE ONLY ERROR + CRITICAL. TRACE/DEBUG/INFO/WARN never leave the
//     device (privacy §2a "Not general logs — only ERROR and CRITICAL").
//   • CONTEXT is session id + build/version + platform ONLY. NO PII — no names,
//     emails, IP-as-identity, hardware fingerprints (privacy §3). The session id
//     is an EPHEMERAL opaque token, the only identifier permitted (privacy §3).
//   • BATCH (buffer N events OR flush every T seconds) and RATE-LIMIT (cap
//     events/interval) so a log storm can't flood the endpoint (D-29 rule 2).
//   • OPT-OUT is honored BEFORE anything is queued (privacy §5): off ⇒ capture
//     nothing, ship nothing.
//   • Serialize to the Sentry-compatible JSON envelope (privacy §2a endpoint).

#ifndef MERIDIAN_TELEMETRY_LOG_CORE_H
#define MERIDIAN_TELEMETRY_LOG_CORE_H

#include <cstdint>
#include <string>
#include <vector>

namespace meridian::telemetry {

// ===========================================================================
// Severity — the client log-level taxonomy.
// ===========================================================================
// Mirrors Godot's logger levels + the C++ modules' spdlog levels (issue #168
// description). The ORDER matters: only Error and Critical are shippable; the
// gate is `severity >= Error`. Trace..Warn are DEVICE-LOCAL, never captured.
enum class Severity : uint8_t {
	Trace    = 0,
	Debug    = 1,
	Info     = 2,
	Warn     = 3,
	Error    = 4,   // ── shippable threshold ──
	Critical = 5,
};

// The one gate that enforces "only ERROR and CRITICAL leave the device"
// (privacy §2a). Everything below Error is dropped at the door.
constexpr bool is_shippable(Severity s) {
	return s >= Severity::Error;
}

// Sentry `level` string for the envelope. Sentry has no "critical"; its highest
// is "fatal" — Critical maps to "fatal", Error to "error" (Sentry-compatible
// envelope, privacy §2a). Lower levels are never serialized (they never reach
// serialization), but the mapping is total for safety.
const char *sentry_level(Severity s);

// ===========================================================================
// SessionContext — the ONLY context attached to a batch. NO PII.
// ===========================================================================
// Exactly the fields privacy §2a permits on the client log channel: "session
// ID, build, and platform context." Nothing else. There is DELIBERATELY no
// field for username / email / account id / IP / hardware id — the type system
// itself is the first line of the no-PII guarantee (the #168 test asserts none
// of those substrings ever appears in a serialized envelope).
//
//   • session_id — EPHEMERAL opaque token for one client run (privacy §3: "an
//     ephemeral session ID" is the ONLY identifier ever permitted). NOT the
//     account, NOT stable across runs. Set by the client at boot; the core
//     treats it as an opaque string and never derives identity from it.
//   • build      — build/version string (e.g. "meridian-client 0.1.0+abcd").
//   • platform   — coarse platform tag ("windows-x64" / "macos-arm64"), matching
//     the D-28 per-platform build shape. NOT a hardware fingerprint.
struct SessionContext {
	std::string session_id;   // ephemeral, opaque (privacy §3)
	std::string build;        // build/version (privacy §2a)
	std::string platform;     // coarse platform tag (privacy §2a)
};

// ===========================================================================
// LogEvent — a single captured ERROR/CRITICAL event.
// ===========================================================================
// The message is a free-form diagnostic string produced by Godot's logger / the
// C++ modules. It carries NO structured PII fields — the caller is responsible
// for not putting PII in the message text (that is a logging-discipline rule,
// documented in the sink), but this core NEVER adds any identifying field of its
// own beyond the SessionContext above.
struct LogEvent {
	Severity    severity = Severity::Error;
	std::string message;            // diagnostic text (no PII by logging discipline)
	std::string logger;             // optional source tag, e.g. "godot" / "net" / "sim"
	uint64_t    timestamp_ms = 0;   // client wall-clock at capture (epoch ms)
};

// ===========================================================================
// CaptureConfig — batching, rate-limit, and opt-out knobs.
// ===========================================================================
struct CaptureConfig {
	// Opt-out (privacy §5). When false, the core captures NOTHING and ships
	// NOTHING — the toggle is honored before any event is queued. Default true
	// mirrors the privacy §5 recommendation (ON by default / opt-out), but the
	// SHIPPED default is the owner's judgment-call #2 — the client setting drives
	// this flag; the core just obeys it.
	bool enabled = true;

	// BATCH size trigger: flush when the buffer reaches this many events.
	uint32_t batch_max_events = 20;

	// BATCH time trigger: flush when this many ms have elapsed since the first
	// buffered event, even if batch_max_events is not reached (bounded latency).
	uint64_t batch_flush_interval_ms = 5000;

	// RATE LIMIT: at most this many CAPTURED events per rate_limit_window_ms.
	// A log storm beyond this is DROPPED at capture (counted, not buffered) so it
	// can never flood the endpoint (D-29 rule 2). Dropped counts surface as a
	// synthetic summary event on the next batch so the drop is observable.
	uint32_t rate_limit_max_events = 50;
	uint64_t rate_limit_window_ms  = 1000;
};

// ===========================================================================
// LogBatch — a rate-limited, size/time-bounded set of events + its context.
// ===========================================================================
// The unit the transport ships. `dropped_by_rate_limit` records how many events
// the rate limiter discarded in the window(s) this batch covers, so the ingest
// side can see storm suppression without the storm itself.
struct LogBatch {
	SessionContext        context;
	std::vector<LogEvent> events;
	uint32_t              dropped_by_rate_limit = 0;

	bool empty() const { return events.empty() && dropped_by_rate_limit == 0; }
};

// ===========================================================================
// Sentry-compatible JSON envelope serialization.
// ===========================================================================
// Serializes a LogBatch to the Sentry envelope shape (privacy §2a "one project-
// hosted, Sentry-compatible endpoint"). The envelope is:
//
//   {"sdk":{...}}\n                         <- envelope header (one line)
//   {"type":"event", ...}\n                 <- item header
//   {"level":"error","message":..., "tags":{session_id,build,platform}, ...}\n
//   ...one event item per captured LogEvent...
//
// A minimal hand-rolled JSON writer (no third-party dep — this core must link
// into the engine-free test with zero deps, same discipline as the movement
// core). Strings are JSON-escaped. The tags carry ONLY session_id/build/platform
// — the no-PII guarantee is structural (SessionContext has no other fields).
std::string serialize_envelope(const LogBatch &batch);

} // namespace meridian::telemetry

#endif // MERIDIAN_TELEMETRY_LOG_CORE_H
