// Project Meridian — client fatal-signal CRASH HANDLER (issue #109).
//
// ─────────────────────────────────────────────────────────────────────────────
//   DECISION: minimal self-contained handler NOW, Google Crashpad seam LATER.
// ─────────────────────────────────────────────────────────────────────────────
// The Client SAD (§5.5, §7.1) names "Crashpad-class out-of-process crash capture
// because Godot ships no reporter". Full Google Crashpad is a large, multi-repo
// vendor drop (mini_chromium + crashpad, its own GN/GYP build, a separate
// out-of-process `crashpad_handler` executable that must be packaged and spawned)
// — not tractable to vendor cleanly into this clean-room Apache-2.0 tree for the
// FIRST test-realm build. Per the issue's pragmatic guidance ("a working crash→
// report→ship path beats a stalled full-Crashpad vendor"), this is a MINIMAL,
// self-contained, in-process POSIX handler: on a fatal signal it captures the
// signal + fault address + backtrace + the no-PII context into a report FILE
// using ONLY async-signal-safe operations, then re-raises the default handler so
// the process still crashes normally. The report is shipped on the NEXT launch
// through the SAME #167/#168 telemetry transport (crash_upload_queue.*).
//
// THE SEAM: install_crash_handler() is the single install point and CrashReport /
// the .mcrash file format / serialize_crash_envelope() are the stable interface.
// Swapping in full Crashpad later means replacing THIS file's signal handler with
// a Crashpad client that writes a minidump + a sidecar .mcrash context file; the
// upload queue, the envelope, the ingest, and the metric are unchanged. On
// platforms without the POSIX path (Windows), install returns false today — that
// is exactly where the Crashpad Windows minidump handler slots in (SAD §5.5).
//
// ASYNC-SIGNAL-SAFETY: the handler calls ONLY async-signal-safe functions
// (open/write/close, clock_gettime, getpid, backtrace, raise) and formats
// integers into stack buffers by hand — NO malloc, NO stdio, NO std::string on
// the crash path. All heap/std::string/std::filesystem work happens in
// install_crash_handler(), which runs at normal boot.

#ifndef MERIDIAN_CRASH_HANDLER_H
#define MERIDIAN_CRASH_HANDLER_H

#include "crash_report_core.h"

#include <string>

namespace meridian::crash {

// Install-time configuration. Copied into fixed async-safe buffers by install();
// the handler reads only those copies (never the std::strings) at crash time.
struct HandlerConfig {
	// Directory the report files are written to (created if absent). Should be a
	// writable per-user path (the binding passes the Godot user:// crash dir).
	std::string crash_dir;

	// The no-PII context stamped into every report (privacy §3). session_id/build/
	// platform ONLY — the same context the #168 log channel ships.
	CrashContext context;

	// Max backtrace frames captured. Clamped to the handler's static buffer size.
	int max_frames = 64;
};

// Install the process-wide fatal-signal handler (SIGSEGV/SIGABRT/SIGBUS/SIGILL/
// SIGFPE). Returns true when a real handler was installed (POSIX build), false on
// an unsupported platform (the Crashpad seam — a no-op stub so callers compile
// and degrade gracefully). Safe to call once at boot; a second call re-applies
// the config. NOT thread-safe against a concurrent crash (install at boot before
// worker threads start, like every crash reporter).
bool install_crash_handler(const HandlerConfig &cfg);

// True when a real (non-stub) handler is active on this platform. Lets the
// binding report honestly whether crash capture is live.
bool crash_handler_active();

} // namespace meridian::crash

#endif // MERIDIAN_CRASH_HANDLER_H
