// Project Meridian — engine-free client CRASH REPORT core (issue #109).
//
// The CLIENT crash channel of the D-29 telemetry triple (docs/telemetry-
// privacy.md §2a: "crash dumps … go to the one project-hosted, Sentry-compatible
// endpoint"; Client SAD §5.5 "Crash reporting → Crashpad-class out-of-process
// capture … project-hosted Sentry-compatible endpoint"). This header is the
// ENGINE-FREE core — plain C++17, NO Godot types, NO third-party deps — exactly
// like telemetry_log_core.* (#168) and the movement controller core (Client SAD
// §9.2). The thin GDExtension binding lives in meridian_crash_reporter.* and the
// async-signal-safe signal handler in crash_handler.*.
//
// ─────────────────────────────────────────────────────────────────────────────
//   WHAT THIS CORE OWNS (the pure, unit-tested logic)
// ─────────────────────────────────────────────────────────────────────────────
//   1. CrashReport — the captured facts of one crash (signal, fault address,
//      timestamp, backtrace frames) + the no-PII SessionContext (privacy §3).
//   2. The ON-DISK report file format — a minimal, line-based ASCII format the
//      async-signal-safe handler writes at crash time (crash_handler.cpp) and
//      this core parses at the NEXT launch. serialize/parse round-trip here is
//      the reference the handler's hand-rolled async-safe writer must match.
//   3. serialize_crash_envelope() — turns a CrashReport into the EXACT Sentry-
//      compatible newline-delimited envelope the #167 ingest already parses, so
//      the crash rides the SAME #167/#168 transport (no new endpoint invented).
//      The crash is a Sentry "fatal" event carrying event_kind=crash so the
//      ingest can count it as meridian_client_crash_total (closing #297).
//
// ─────────────────────────────────────────────────────────────────────────────
//   THE PRIVACY CONTRACT (docs/telemetry-privacy.md §3 — enforced structurally)
// ─────────────────────────────────────────────────────────────────────────────
//   The ONLY context attached to a crash is the shared no-PII SessionContext
//   (session_id / build / platform). There is DELIBERATELY no field for a
//   username / email / IP / hardware id — the type system is the first line of
//   the no-PII guarantee, identical to the #168 log channel. The backtrace
//   carries return ADDRESSES (and optionally symbol names), never user data.

#ifndef MERIDIAN_CRASH_REPORT_CORE_H
#define MERIDIAN_CRASH_REPORT_CORE_H

#include "telemetry_log_core.h"   // meridian::telemetry::SessionContext (shared no-PII context)

#include <cstdint>
#include <string>
#include <vector>

namespace meridian::crash {

// The crash channel reuses the #168 no-PII context verbatim — one SessionContext
// shape across the whole client telemetry triple (privacy §2a: "session ID,
// build, and platform context"; nothing else).
using CrashContext = telemetry::SessionContext;

// ===========================================================================
// CrashReport — the captured facts of one crash.
// ===========================================================================
// Produced two ways: (a) the async-signal-safe handler writes a report FILE at
// crash time and this struct is rebuilt from it at the next launch, or (b) a test
// constructs one directly. Everything here is either a fixed-width fact of the
// crash or the no-PII context — never user data.
struct CrashReport {
	// The delivering signal (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE …). 0 if unknown.
	int signal_number = 0;

	// Human-readable signal name ("SIGSEGV" …), filled by parse_report_file() from
	// signal_number. Convenience for diagnostics; never authoritative over the number.
	std::string signal_name;

	// The fault address (POSIX siginfo si_addr), best-effort. 0 when not applicable
	// (e.g. SIGABRT). Stored as an integer so the format is toolchain-independent.
	uint64_t fault_address = 0;

	// Client wall-clock (epoch ms) at crash time. 0 if the handler could not read a
	// clock (kept best-effort — a crash handler must never depend on much).
	uint64_t timestamp_ms = 0;

	// The no-PII context (privacy §3). Filled from the values the client supplied at
	// install() time — the handler stores them in async-safe buffers.
	CrashContext context;

	// The backtrace: raw return addresses, innermost first. Symbolisation is done
	// (best-effort) at UPLOAD time, off the crash path — the handler only captures
	// addresses (backtrace() is async-signal-safe; backtrace_symbols() is NOT).
	std::vector<uint64_t> frames;

	// Optional human-readable symbol strings, one per frame, filled at upload time
	// (empty when unsymbolised). Never contains PII — module + symbol + offset only.
	std::vector<std::string> symbols;

	bool empty() const { return signal_number == 0 && frames.empty(); }
};

// The POSIX signal name for the report ("SIGSEGV" …). Returns "SIG?" for an
// unknown number so the format is always well-formed.
const char *signal_name(int sig);

// A one-line human summary used as the Sentry event `message`
// (e.g. "SIGSEGV (signal 11) at 0x0 — 7 frames"). No PII.
std::string crash_summary(const CrashReport &r);

// ===========================================================================
// On-disk report file format (v1) — line-based ASCII.
// ===========================================================================
// The handler writes this with an async-signal-safe hand-rolled writer
// (crash_handler.cpp); this core's serialize_report_file() is the REFERENCE
// producer (used by tests + any non-crash-path caller) and parse_report_file()
// is the reader used at the next launch. The format:
//
//   MERIDIAN-CRASH v1\n
//   sig <decimal>\n
//   addr <hex-without-0x>\n
//   time <decimal-ms>\n
//   sid <session_id>\n
//   build <build>\n
//   plat <platform>\n
//   fr <hex-without-0x>\n        (repeated, innermost first)
//   ...
//
// Values (sid/build/plat) are written verbatim to end-of-line; they are
// operator/build-controlled tokens that never contain a newline (the session id
// is an opaque token, build/platform are build tags). Unknown keys are ignored
// on parse (forward-compat). A file whose first line is not the v1 magic fails.
std::string serialize_report_file(const CrashReport &r);
bool        parse_report_file(const std::string &text, CrashReport &out);

// ===========================================================================
// Sentry-compatible envelope — the SAME shape #167/#168 already parse.
// ===========================================================================
// Serialises a CrashReport into a newline-delimited Sentry envelope that the
// telemetryd ingest (server/telemetryd/ingest.*) accepts unchanged:
//
//   {"sdk":{"name":"meridian.client.telemetry","version":"1"}}\n   <- header
//   {"type":"event","content_type":"application/json"}\n           <- item hdr
//   {"level":"fatal","message":"<summary>","timestamp":N,"logger":"crash",
//    "event_kind":"crash","frames":["0x..",..],"tags":{session_id,build,
//    platform}}\n                                                   <- payload
//
// `event_kind:"crash"` is the marker the ingest keys on to count the crash as
// meridian_client_crash_total (it is a top-level payload field, NOT a tag, so it
// passes the ingest's tag allow-list; it is not a PII-shaped key). `level` is
// "fatal" (Sentry's highest — Critical maps here) so the crash sorts with the
// most severe events. The tags carry ONLY the no-PII context.
std::string serialize_crash_envelope(const CrashReport &r);

} // namespace meridian::crash

#endif // MERIDIAN_CRASH_REPORT_CORE_H
