// Project Meridian — controlled CRASH HANDLER smoke test (issue #109).
//
// Proves the REAL fatal-signal handler (crash_handler.cpp) fires and writes a
// parseable .mcrash report, WITHOUT killing the test runner: each crash is
// triggered in a forked CHILD process. The parent installs nothing, forks, the
// child installs the handler + deliberately crashes (null deref → SIGSEGV, and
// abort() → SIGABRT), the handler writes the report and re-raises the default
// disposition so the child dies with the signal; the parent then asserts a report
// file exists in the temp crash dir, parses, and carries the right signal +
// no-PII context + a non-empty backtrace.
//
// This is the "controlled crash test" the issue asks for: the true signal-handler
// firing IS exercised here (in a subprocess). A real END-USER crash of the full
// Godot client still needs owner/runtime confirmation (this proves the mechanism,
// not the engine integration). POSIX-only; on other platforms it SKIPs (exit 0).

#include "crash_report_core.h"
#include "crash_upload_queue.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#define MERIDIAN_CRASH_SMOKE_POSIX 1
#else
#define MERIDIAN_CRASH_SMOKE_POSIX 0
#endif

#if MERIDIAN_CRASH_SMOKE_POSIX
#include "crash_handler.h"
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace cr = meridian::crash;
namespace fs = std::filesystem;

static int g_fail = 0;
static void check(const char *name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

// Force a real fault the optimiser cannot elide.
static void trigger_segv() {
	volatile int *p = nullptr;
	*p = 42;              // SIGSEGV
}

// Run one crash scenario in a forked child. `kind`: 0 = SIGSEGV, 1 = SIGABRT.
// Returns the wait status via out-param; the child never returns normally.
static void run_child_crash(const std::string &dir, int kind) {
	// Flush our stdio buffer BEFORE forking so the child does not inherit (and, on
	// an abnormal exit, re-emit) the parent's buffered log lines.
	std::fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		// ── child ──
		cr::HandlerConfig cfg;
		cfg.crash_dir = dir;
		cfg.context.session_id = "smoke-session";
		cfg.context.build = "smoke-build";
		cfg.context.platform = "smoke-platform";
		cfg.max_frames = 32;
		cr::install_crash_handler(cfg);
		if (kind == 0) trigger_segv();
		else           std::abort();   // SIGABRT
		_exit(0);   // unreachable if the handler + re-raise work
	}
	int status = 0;
	waitpid(pid, &status, 0);
	// The child must have died by a signal (the handler re-raised the default).
	check(kind == 0 ? "child died by signal (segv)" : "child died by signal (abrt)",
	      WIFSIGNALED(status));
}

// Find the single report file in `dir` and parse it.
static bool load_only_report(const std::string &dir, cr::CrashReport &out) {
	cr::CrashReportQueue queue(dir);
	auto files = queue.pending_files();
	if (files.size() != 1) return false;
	return cr::CrashReportQueue::load(files[0], out);
}

int main() {
	std::printf("crash-handler-smoke-test\n");

	// ── SIGSEGV scenario ──
	{
		fs::path dir = fs::temp_directory_path() / "meridian-crash-smoke-segv";
		std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
		run_child_crash(dir.string(), /*kind=*/0);

		cr::CrashReport r;
		bool loaded = load_only_report(dir.string(), r);
		check("segv: exactly one report written + parsed", loaded);
		if (loaded) {
			check("segv: signal is SIGSEGV(11)", r.signal_number == SIGSEGV);
			check("segv: context session preserved", r.context.session_id == "smoke-session");
			check("segv: context build preserved", r.context.build == "smoke-build");
			check("segv: context platform preserved", r.context.platform == "smoke-platform");
			check("segv: backtrace captured", !r.frames.empty());
			check("segv: timestamp set", r.timestamp_ms > 0);
		}
		fs::remove_all(dir, ec);
	}

	// ── SIGABRT scenario ──
	{
		fs::path dir = fs::temp_directory_path() / "meridian-crash-smoke-abrt";
		std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
		run_child_crash(dir.string(), /*kind=*/1);

		cr::CrashReport r;
		bool loaded = load_only_report(dir.string(), r);
		check("abrt: exactly one report written + parsed", loaded);
		if (loaded) {
			check("abrt: signal is SIGABRT(6)", r.signal_number == SIGABRT);
			check("abrt: backtrace captured", !r.frames.empty());
		}
		fs::remove_all(dir, ec);
	}

	if (g_fail == 0) {
		std::printf("crash-handler-smoke-test: ALL PASS\n");
		return 0;
	}
	std::printf("crash-handler-smoke-test: %d FAILURE(S)\n", g_fail);
	return 1;
}

#else  // non-POSIX: the Crashpad seam platform — nothing to exercise here yet.

int main() {
	std::printf("crash-handler-smoke-test: SKIP (no POSIX signal handler on this platform)\n");
	return 0;
}

#endif
