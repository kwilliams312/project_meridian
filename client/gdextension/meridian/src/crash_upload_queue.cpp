// Project Meridian — client crash upload queue (issue #109). Engine-free; uses
// std::filesystem (this runs at normal launch, NOT on the crash path).

#include "crash_upload_queue.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace meridian::crash {

namespace fs = std::filesystem;

std::vector<std::string> CrashReportQueue::pending_files() const {
	std::vector<std::string> out;
	std::error_code ec;
	if (!fs::exists(dir_, ec) || !fs::is_directory(dir_, ec)) {
		return out;   // no directory yet ⇒ nothing pending (degrade gracefully)
	}
	for (const auto &entry : fs::directory_iterator(dir_, ec)) {
		if (ec) break;
		if (!entry.is_regular_file()) continue;
		const fs::path &p = entry.path();
		if (p.extension() == kReportExtension) {
			out.push_back(p.string());
		}
	}
	// Oldest-first by filename (the handler encodes a monotonic seq in the name).
	std::sort(out.begin(), out.end());
	return out;
}

bool CrashReportQueue::load(const std::string &path, CrashReport &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	return parse_report_file(ss.str(), out);
}

bool CrashReportQueue::remove_file(const std::string &path) {
	std::error_code ec;
	return fs::remove(path, ec) && !ec;
}

UploadSweepResult upload_pending(const CrashReportQueue &queue, telemetry::ITransport &transport) {
	UploadSweepResult res;
	for (const std::string &path : queue.pending_files()) {
		++res.found;
		CrashReport report;
		if (!CrashReportQueue::load(path, report)) {
			// Unparseable — it can never succeed; drop it so the queue can't wedge.
			++res.malformed;
			CrashReportQueue::remove_file(path);
			continue;
		}
		const std::string envelope = serialize_crash_envelope(report);
		const telemetry::ShipResult r = transport.ship(envelope);
		switch (r) {
			case telemetry::ShipResult::Ok:
				++res.shipped;
				CrashReportQueue::remove_file(path);   // delivered ⇒ delete
				break;
			case telemetry::ShipResult::Failed:
				++res.failed;                          // keep for a later attempt
				break;
			case telemetry::ShipResult::NoSink:
				++res.no_sink;                         // no endpoint ⇒ keep
				break;
		}
	}
	return res;
}

} // namespace meridian::crash
