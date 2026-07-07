// Project Meridian — MeridianCrashReporter GDExtension binding (issue #109).
// Thin Godot wrapper over the tested engine-free crash cores. All policy lives in
// the cores; this file marshals types, owns the config-injected transport (the
// SAME GodotHttpTransport the #168 log channel uses), and keeps the client-side
// crash counter.

#include "meridian_crash_reporter.h"

#include "crash_handler.h"
#include "crash_report_core.h"
#include "crash_upload_queue.h"
#include "godot_http_transport.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

using namespace godot;

namespace meridian {

namespace {

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

} // namespace

MeridianCrashReporter::MeridianCrashReporter() {
	// Default: no endpoint ⇒ NullTransport (reports are kept on disk until an
	// endpoint is configured; telemetry is never a hard dependency, privacy §6).
	transport_ = std::make_unique<telemetry::NullTransport>();
}

MeridianCrashReporter::~MeridianCrashReporter() = default;

bool MeridianCrashReporter::install(const String &session_id, const String &build,
                                    const String &platform, const String &crash_dir) {
	crash_dir_ = crash_dir;

	crash::HandlerConfig cfg;
	cfg.crash_dir            = to_std(crash_dir);
	cfg.context.session_id   = to_std(session_id);
	cfg.context.build        = to_std(build);
	cfg.context.platform     = to_std(platform);

	installed_ = crash::install_crash_handler(cfg);
	if (!installed_) {
		// Not fatal: on a platform without the POSIX path the handler is a no-op
		// stub (the Crashpad seam). We can still flush reports left by other runs.
		UtilityFunctions::push_warning(
			"MeridianCrashReporter: fatal-signal capture unavailable on this platform "
			"(crash reports will not be generated here; upload of existing reports still works).");
	}
	return installed_;
}

bool MeridianCrashReporter::is_active() const {
	return crash::crash_handler_active();
}

void MeridianCrashReporter::set_endpoint(const String &url) {
	endpoint_url_ = url;
	if (url.strip_edges().is_empty()) {
		transport_ = std::make_unique<telemetry::NullTransport>();
		return;
	}
	auto http = std::make_unique<GodotHttpTransport>(url);
	if (!http->has_endpoint()) {
		UtilityFunctions::push_warning(
			String("MeridianCrashReporter: unusable endpoint URL, using no-op sink: ") + url);
		transport_ = std::make_unique<telemetry::NullTransport>();
		return;
	}
	transport_ = std::move(http);
}

String MeridianCrashReporter::get_endpoint() const {
	return endpoint_url_;
}

int MeridianCrashReporter::flush_pending() {
	if (crash_dir_.strip_edges().is_empty() || !transport_) {
		return 0;
	}
	crash::CrashReportQueue queue(to_std(crash_dir_));
	crash::UploadSweepResult r = crash::upload_pending(queue, *transport_);
	crash_found_total_   += r.found;
	crash_shipped_total_ += r.shipped;
	return static_cast<int>(r.shipped);
}

int MeridianCrashReporter::pending_count() const {
	if (crash_dir_.strip_edges().is_empty()) return 0;
	crash::CrashReportQueue queue(to_std(crash_dir_));
	return static_cast<int>(queue.pending_count());
}

Dictionary MeridianCrashReporter::get_stats() const {
	Dictionary d;
	d["active"]              = crash::crash_handler_active();
	d["has_endpoint"]        = transport_ ? transport_->has_endpoint() : false;
	d["pending"]             = pending_count();
	d["crash_shipped_total"] = static_cast<int64_t>(crash_shipped_total_);
	d["crash_found_total"]   = static_cast<int64_t>(crash_found_total_);
	d["crash_dir"]           = crash_dir_;
	return d;
}

void MeridianCrashReporter::_bind_methods() {
	ClassDB::bind_method(
		D_METHOD("install", "session_id", "build", "platform", "crash_dir"),
		&MeridianCrashReporter::install);
	ClassDB::bind_method(D_METHOD("is_active"), &MeridianCrashReporter::is_active);
	ClassDB::bind_method(D_METHOD("set_endpoint", "url"), &MeridianCrashReporter::set_endpoint);
	ClassDB::bind_method(D_METHOD("get_endpoint"), &MeridianCrashReporter::get_endpoint);
	ClassDB::bind_method(D_METHOD("flush_pending"), &MeridianCrashReporter::flush_pending);
	ClassDB::bind_method(D_METHOD("pending_count"), &MeridianCrashReporter::pending_count);
	ClassDB::bind_method(D_METHOD("get_stats"), &MeridianCrashReporter::get_stats);
}

} // namespace meridian
