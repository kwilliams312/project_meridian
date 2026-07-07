// Project Meridian — client fatal-signal crash handler (issue #109).
// See crash_handler.h for the DECISION (minimal handler now, Crashpad seam
// later) and the async-signal-safety contract this file honours.

#include "crash_handler.h"

#include <cstring>

// ── Platform gate ────────────────────────────────────────────────────────────
// The POSIX signal path targets macOS + Linux (the M0 client platforms; the task
// scopes macOS minidump capture). On other platforms install returns false — the
// documented seam where a Crashpad Windows minidump handler slots in (SAD §5.5).
#if defined(__APPLE__) || defined(__linux__)
#define MERIDIAN_CRASH_POSIX 1
#else
#define MERIDIAN_CRASH_POSIX 0
#endif

#if MERIDIAN_CRASH_POSIX
#include <cerrno>
#include <csignal>
#include <ctime>
#include <execinfo.h>   // backtrace()
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#endif

namespace meridian::crash {

#if MERIDIAN_CRASH_POSIX

namespace {

// ── Async-safe global state (set at install, read in the handler) ────────────
// Fixed-size buffers so the handler never touches the heap. install() copies the
// std::string config into these; the handler reads ONLY these.
constexpr std::size_t kDirMax   = 1024;
constexpr std::size_t kSidMax   = 256;
constexpr std::size_t kBuildMax = 256;
constexpr std::size_t kPlatMax  = 128;
constexpr int         kFrameCap = 128;

char  g_dir[kDirMax]     = {0};
char  g_sid[kSidMax]     = {0};
char  g_build[kBuildMax] = {0};
char  g_plat[kPlatMax]   = {0};
int   g_max_frames       = 64;
bool  g_active           = false;

// Backtrace scratch — written only inside the handler (single-shot; the process
// is dying). Static so backtrace() has no need to allocate.
void *g_frames[kFrameCap];

// A monotonic per-process sequence so two crashes (or a crash + re-raise) never
// collide on a filename. sig_atomic_t is safe to touch in a handler.
volatile std::sig_atomic_t g_seq = 0;

// Re-entrancy guard: if the handler itself faults, bail immediately.
volatile std::sig_atomic_t g_in_handler = 0;

// The fatal signals we install for.
constexpr int kFatalSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};

// ── Async-signal-safe formatting + write helpers ─────────────────────────────
void safe_write(int fd, const char *buf, std::size_t n) {
	while (n > 0) {
		ssize_t w = ::write(fd, buf, n);
		if (w <= 0) {
			if (w < 0 && errno == EINTR) continue;
			return;   // give up quietly — a crash handler must not loop forever
		}
		buf += w;
		n -= static_cast<std::size_t>(w);
	}
}

void safe_write_cstr(int fd, const char *s) { safe_write(fd, s, std::strlen(s)); }

// Write an unsigned decimal (async-safe: no snprintf/stdio).
void write_dec(int fd, unsigned long long v) {
	char tmp[24];
	int i = static_cast<int>(sizeof(tmp));
	if (v == 0) { tmp[--i] = '0'; }
	while (v > 0 && i > 0) { tmp[--i] = static_cast<char>('0' + (v % 10)); v /= 10; }
	safe_write(fd, tmp + i, sizeof(tmp) - static_cast<std::size_t>(i));
}

// Write an unsigned hex value (no 0x prefix) — matches the .mcrash file format.
void write_hex(int fd, unsigned long long v) {
	static const char kHex[] = "0123456789abcdef";
	char tmp[18];
	int i = static_cast<int>(sizeof(tmp));
	if (v == 0) { tmp[--i] = '0'; }
	while (v > 0 && i > 0) { tmp[--i] = kHex[v & 0xF]; v >>= 4; }
	safe_write(fd, tmp + i, sizeof(tmp) - static_cast<std::size_t>(i));
}

// Append a decimal to the filename buffer being built (async-safe). Returns the
// new end pointer. Caller guarantees room.
char *append_dec(char *p, char *end, unsigned long long v) {
	char tmp[24];
	int i = static_cast<int>(sizeof(tmp));
	if (v == 0) { tmp[--i] = '0'; }
	while (v > 0 && i > 0) { tmp[--i] = static_cast<char>('0' + (v % 10)); v /= 10; }
	for (int k = i; k < static_cast<int>(sizeof(tmp)) && p < end - 1; ++k) *p++ = tmp[k];
	return p;
}

char *append_cstr(char *p, char *end, const char *s) {
	while (*s && p < end - 1) *p++ = *s++;
	return p;
}

// Current wall-clock in epoch ms via clock_gettime (async-signal-safe).
unsigned long long now_ms_safe() {
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
	return static_cast<unsigned long long>(ts.tv_sec) * 1000ull +
	       static_cast<unsigned long long>(ts.tv_nsec) / 1000000ull;
}

// The fatal-signal handler. Everything here is async-signal-safe.
void handle_fatal(int sig, siginfo_t *info, void * /*ucontext*/) {
	if (g_in_handler) {
		// A fault inside the handler — stop trying, die with the signal default.
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}
	g_in_handler = 1;
	const int seq = static_cast<int>(g_seq++);

	// Build the report path: <dir>/crash-<pid>-<sig>-<seq>.mcrash  (async-safe).
	char path[kDirMax + 64];
	char *p = path;
	char *pend = path + sizeof(path);
	p = append_cstr(p, pend, g_dir);
	p = append_cstr(p, pend, "/crash-");
	p = append_dec(p, pend, static_cast<unsigned long long>(getpid()));
	p = append_cstr(p, pend, "-");
	p = append_dec(p, pend, static_cast<unsigned long long>(sig));
	p = append_cstr(p, pend, "-");
	p = append_dec(p, pend, static_cast<unsigned long long>(seq));
	p = append_cstr(p, pend, ".mcrash");
	*p = '\0';

	int fd = ::open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd >= 0) {
		safe_write_cstr(fd, "MERIDIAN-CRASH v1\n");
		safe_write_cstr(fd, "sig ");
		write_dec(fd, static_cast<unsigned long long>(sig));
		safe_write_cstr(fd, "\n");
		safe_write_cstr(fd, "addr ");
		write_hex(fd, info ? reinterpret_cast<unsigned long long>(info->si_addr) : 0ull);
		safe_write_cstr(fd, "\n");
		safe_write_cstr(fd, "time ");
		write_dec(fd, now_ms_safe());
		safe_write_cstr(fd, "\n");
		safe_write_cstr(fd, "sid ");   safe_write_cstr(fd, g_sid);   safe_write_cstr(fd, "\n");
		safe_write_cstr(fd, "build "); safe_write_cstr(fd, g_build); safe_write_cstr(fd, "\n");
		safe_write_cstr(fd, "plat ");  safe_write_cstr(fd, g_plat);  safe_write_cstr(fd, "\n");

		int want = g_max_frames;
		if (want > kFrameCap) want = kFrameCap;
		if (want < 0) want = 0;
		int n = backtrace(g_frames, want);
		for (int i = 0; i < n; ++i) {
			safe_write_cstr(fd, "fr ");
			write_hex(fd, reinterpret_cast<unsigned long long>(g_frames[i]));
			safe_write_cstr(fd, "\n");
		}
		::close(fd);
	}

	// Re-raise with the default disposition so the process dies as it normally
	// would (correct exit signal + any OS core-dump behaviour).
	signal(sig, SIG_DFL);
	raise(sig);
}

void copy_clamped(char *dst, std::size_t cap, const std::string &src) {
	std::size_t n = src.size();
	if (n >= cap) n = cap - 1;
	std::memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

} // namespace

bool install_crash_handler(const HandlerConfig &cfg) {
	// Ensure the crash directory exists (normal-boot context — heap/fs OK here).
	{
		std::error_code ec;
		std::filesystem::create_directories(cfg.crash_dir, ec);
		// If we cannot create it, still install: open() in the handler will just
		// fail and re-raise — no worse than no handler, and the dir may appear later.
	}

	copy_clamped(g_dir,   kDirMax,   cfg.crash_dir);
	copy_clamped(g_sid,   kSidMax,   cfg.context.session_id);
	copy_clamped(g_build, kBuildMax, cfg.context.build);
	copy_clamped(g_plat,  kPlatMax,  cfg.context.platform);
	g_max_frames = cfg.max_frames > 0 ? cfg.max_frames : 0;

	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = &handle_fatal;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);

	bool all_ok = true;
	for (int s : kFatalSignals) {
		if (sigaction(s, &sa, nullptr) != 0) all_ok = false;
	}
	g_active = all_ok;
	return all_ok;
}

bool crash_handler_active() { return g_active; }

#else // !MERIDIAN_CRASH_POSIX — the Crashpad seam (Windows minidump handler slots here)

bool install_crash_handler(const HandlerConfig & /*cfg*/) { return false; }
bool crash_handler_active() { return false; }

#endif // MERIDIAN_CRASH_POSIX

} // namespace meridian::crash
