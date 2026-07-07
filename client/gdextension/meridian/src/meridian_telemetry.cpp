// Project Meridian — MeridianTelemetry GDExtension binding (issue #168).
// Thin Godot wrapper over the tested engine-free telemetry log capture core.
// All policy lives in the core; this file marshals types, owns the wall-clock,
// and owns the config-injected HTTP transport (the #167 ingest endpoint).

#include "meridian_telemetry.h"

#include "godot_http_transport.h"
#include "telemetry_log_core.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

using namespace godot;

namespace meridian {

namespace {

// Convert a Godot String (UTF-8) to std::string for the engine-free core.
std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

} // namespace

MeridianTelemetry::MeridianTelemetry() {
	// Default: enabled pipeline (privacy §5 recommendation — opt-out), NullTransport
	// sink until an endpoint is configured. Context is empty until configure().
	telemetry::CaptureConfig cfg;   // defaults per CaptureConfig
	telemetry::SessionContext ctx;  // empty until configure()
	capture_ = std::make_unique<telemetry::LogCapture>(cfg, ctx);
	transport_ = std::make_unique<telemetry::NullTransport>();
}

MeridianTelemetry::~MeridianTelemetry() = default;

uint64_t MeridianTelemetry::now_ms() const {
	Time *t = Time::get_singleton();
	return t ? static_cast<uint64_t>(t->get_ticks_msec()) : 0;
}

void MeridianTelemetry::configure(const String &session_id, const String &build,
                                  const String &platform) {
	// Rebuild the capture core with the no-PII context. Preserve the current
	// enabled flag + knobs by reading them back off the existing core's config is
	// not exposed; simplest correct path is to reconstruct with a fresh config
	// carrying the current enabled state.
	const bool was_enabled = capture_ ? capture_->enabled() : true;
	telemetry::CaptureConfig cfg;
	cfg.enabled = was_enabled;
	telemetry::SessionContext ctx;
	ctx.session_id = to_std(session_id);
	ctx.build      = to_std(build);
	ctx.platform   = to_std(platform);
	capture_ = std::make_unique<telemetry::LogCapture>(cfg, ctx);
}

void MeridianTelemetry::set_batching(int batch_max_events, int batch_flush_interval_ms) {
	// Reconstruct with new batching knobs, preserving context + enabled + rate.
	// (CaptureConfig is small; a full setter surface is unnecessary at M0.)
	if (!capture_) return;
	telemetry::CaptureConfig cfg;
	cfg.enabled = capture_->enabled();
	cfg.batch_max_events = static_cast<uint32_t>(batch_max_events > 0 ? batch_max_events : 1);
	cfg.batch_flush_interval_ms = static_cast<uint64_t>(batch_flush_interval_ms > 0 ? batch_flush_interval_ms : 1);
	telemetry::SessionContext ctx = capture_->context();
	capture_ = std::make_unique<telemetry::LogCapture>(cfg, ctx);
}

void MeridianTelemetry::set_rate_limit(int max_events, int window_ms) {
	if (!capture_) return;
	telemetry::CaptureConfig cfg;
	cfg.enabled = capture_->enabled();
	cfg.rate_limit_max_events = static_cast<uint32_t>(max_events > 0 ? max_events : 1);
	cfg.rate_limit_window_ms  = static_cast<uint64_t>(window_ms > 0 ? window_ms : 1);
	telemetry::SessionContext ctx = capture_->context();
	capture_ = std::make_unique<telemetry::LogCapture>(cfg, ctx);
}

void MeridianTelemetry::set_enabled(bool enabled) {
	if (capture_) capture_->set_enabled(enabled);
}

bool MeridianTelemetry::is_enabled() const {
	return capture_ && capture_->enabled();
}

void MeridianTelemetry::set_endpoint(const String &url) {
	endpoint_url_ = url;
	if (url.strip_edges().is_empty()) {
		transport_ = std::make_unique<telemetry::NullTransport>();
		return;
	}
	auto http = std::make_unique<GodotHttpTransport>(url);
	if (!http->has_endpoint()) {
		// Malformed URL -> degrade to no-op sink rather than fail hard.
		UtilityFunctions::push_warning(
			String("MeridianTelemetry: unusable endpoint URL, using no-op sink: ") + url);
		transport_ = std::make_unique<telemetry::NullTransport>();
		return;
	}
	transport_ = std::move(http);
}

String MeridianTelemetry::get_endpoint() const {
	return endpoint_url_;
}

bool MeridianTelemetry::capture_log(int severity, const String &message,
                                    const String &logger) {
	if (!capture_) return false;
	telemetry::LogEvent ev;
	ev.severity     = static_cast<telemetry::Severity>(severity);
	ev.message      = to_std(message);
	ev.logger       = to_std(logger);
	ev.timestamp_ms = now_ms();
	return capture_->capture(ev, ev.timestamp_ms);
}

bool MeridianTelemetry::poll() {
	if (!capture_ || !transport_) return false;
	return capture_->flush_and_ship(*transport_, now_ms());
}

bool MeridianTelemetry::flush_now() {
	if (!capture_ || !transport_) return false;
	telemetry::LogBatch batch = capture_->flush();
	if (batch.empty()) return false;
	capture_->ship(batch, *transport_);
	return true;
}

Dictionary MeridianTelemetry::get_stats() const {
	Dictionary d;
	if (capture_) {
		d["buffered"]        = static_cast<int64_t>(capture_->buffered_count());
		d["pending_dropped"] = static_cast<int64_t>(capture_->pending_dropped());
		d["captured"]        = static_cast<int64_t>(capture_->lifetime_captured());
		d["dropped"]         = static_cast<int64_t>(capture_->lifetime_dropped());
		d["enabled"]         = capture_->enabled();
	}
	d["has_endpoint"] = transport_ ? transport_->has_endpoint() : false;
	return d;
}

void MeridianTelemetry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "session_id", "build", "platform"),
	                     &MeridianTelemetry::configure);
	ClassDB::bind_method(D_METHOD("set_batching", "batch_max_events", "batch_flush_interval_ms"),
	                     &MeridianTelemetry::set_batching);
	ClassDB::bind_method(D_METHOD("set_rate_limit", "max_events", "window_ms"),
	                     &MeridianTelemetry::set_rate_limit);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &MeridianTelemetry::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &MeridianTelemetry::is_enabled);
	ClassDB::bind_method(D_METHOD("set_endpoint", "url"), &MeridianTelemetry::set_endpoint);
	ClassDB::bind_method(D_METHOD("get_endpoint"), &MeridianTelemetry::get_endpoint);
	ClassDB::bind_method(D_METHOD("capture_log", "severity", "message", "logger"),
	                     &MeridianTelemetry::capture_log);
	ClassDB::bind_method(D_METHOD("poll"), &MeridianTelemetry::poll);
	ClassDB::bind_method(D_METHOD("flush_now"), &MeridianTelemetry::flush_now);
	ClassDB::bind_method(D_METHOD("get_stats"), &MeridianTelemetry::get_stats);

	BIND_ENUM_CONSTANT(SEVERITY_TRACE);
	BIND_ENUM_CONSTANT(SEVERITY_DEBUG);
	BIND_ENUM_CONSTANT(SEVERITY_INFO);
	BIND_ENUM_CONSTANT(SEVERITY_WARN);
	BIND_ENUM_CONSTANT(SEVERITY_ERROR);
	BIND_ENUM_CONSTANT(SEVERITY_CRITICAL);
}

} // namespace meridian
