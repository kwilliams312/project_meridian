// Project Meridian — client crash UPLOAD QUEUE (issue #109). Engine-free.
//
// The "ship on this-or-next launch" half of the crash channel. The async-signal-
// safe handler (crash_handler.*) writes a report FILE at crash time; at the NEXT
// launch this queue scans the crash directory, rebuilds each CrashReport
// (crash_report_core.*), serialises it to the Sentry envelope, and ships it
// through the SAME telemetry ITransport seam the #168 log channel uses
// (telemetry_transport.h) — reusing the #167/#168 transport, inventing no new
// endpoint. A report is deleted only after the transport ACCEPTS it (Ok); a
// Failed/NoSink leaves the file for a later attempt (bounded retry across
// launches — a dead endpoint never loses a crash, never grows unbounded because
// the file count is capped by the max-retained policy at write time).
//
// NOT on the crash path — this runs at normal launch, so it may use
// std::filesystem + normal allocation freely (unlike crash_handler.cpp, which is
// strictly async-signal-safe). Deterministic + injectable transport, so the
// whole upload path is unit-tested with a MockTransport against a temp dir.

#ifndef MERIDIAN_CRASH_UPLOAD_QUEUE_H
#define MERIDIAN_CRASH_UPLOAD_QUEUE_H

#include "crash_report_core.h"
#include "telemetry_transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace meridian::crash {

// The on-disk extension for a queued crash report file. The handler writes
// "crash-<pid>-<sig>-<seq>.mcrash" into the crash directory; the queue globs "*.mcrash".
inline constexpr const char *kReportExtension = ".mcrash";

// Outcome of one upload_pending() sweep — enough for the binding to surface a
// client-side crash counter (the "client-side equivalent" of
// meridian_client_crash_total, #297) and for tests to assert behaviour.
struct UploadSweepResult {
	uint32_t found     = 0;   // report files discovered this sweep
	uint32_t shipped   = 0;   // reports the transport ACCEPTED (Ok) and we deleted
	uint32_t failed    = 0;   // reports the transport could not take (Failed) — kept
	uint32_t no_sink   = 0;   // no endpoint configured — kept for a later launch
	uint32_t malformed = 0;   // unparseable files — deleted (can never succeed)
};

// The crash-report directory scanner + uploader.
class CrashReportQueue {
public:
	explicit CrashReportQueue(std::string dir) : dir_(std::move(dir)) {}

	const std::string &dir() const { return dir_; }

	// Pending report file paths, sorted ascending by filename (oldest sequence
	// first). Empty when the directory is absent or has no *.mcrash files.
	std::vector<std::string> pending_files() const;

	// Number of pending report files (cheap-ish diagnostic).
	std::size_t pending_count() const { return pending_files().size(); }

	// Parse one report file from disk into `out`. Returns false if the file is
	// missing or is not a well-formed v1 report.
	static bool load(const std::string &path, CrashReport &out);

	// Delete a report file (after a successful upload, or when it is malformed).
	static bool remove_file(const std::string &path);

private:
	std::string dir_;
};

// Sweep the queue: for each pending report, load → serialise_crash_envelope →
// transport.ship(). On Ok delete the file (uploaded); on Failed/NoSink keep it
// for a later launch; a malformed file is deleted (it can never succeed). Returns
// the tally. Deterministic given the directory + transport, so tests drive it
// with a MockTransport (assert the exact envelope was shipped) + a temp dir.
UploadSweepResult upload_pending(const CrashReportQueue &queue, telemetry::ITransport &transport);

} // namespace meridian::crash

#endif // MERIDIAN_CRASH_UPLOAD_QUEUE_H
