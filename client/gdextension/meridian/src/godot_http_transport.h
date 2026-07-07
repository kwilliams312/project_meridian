// Project Meridian — the Godot HTTP telemetry transport (issues #168, #109).
//
// The ONE real ITransport implementation: it POSTs a serialized Sentry-compatible
// envelope to the project-hosted ingest endpoint (#167) OFF the game thread via
// the engine's WorkerThreadPool ("never block the game loop", #168). Extracted
// from meridian_telemetry.cpp so BOTH client telemetry channels reuse the SAME
// transport, byte-for-byte:
//   • the #168 ERROR/CRITICAL log channel  (MeridianTelemetry), and
//   • the #109 crash channel               (MeridianCrashReporter),
// which is exactly the issue's "reuse the #167/#168 transport, don't invent a new
// one" requirement — the crash envelope rides the identical POST path.
//
// This is the ONLY file in the crash/telemetry stack that depends on godot-cpp;
// the policy cores (telemetry_log_core, crash_report_core, crash_upload_queue)
// stay engine-free and unit-tested through the ITransport seam with a MockTransport.

#ifndef MERIDIAN_GODOT_HTTP_TRANSPORT_H
#define MERIDIAN_GODOT_HTTP_TRANSPORT_H

#include "telemetry_transport.h"

#include <godot_cpp/variant/string.hpp>

namespace meridian {

// Ships an envelope to a Sentry-compatible endpoint over HTTP, off the main/game
// thread. Fire-and-forget: ship() enqueues the POST on a worker task and returns
// Ok immediately (a malformed/unset URL leaves has_endpoint() false → NoSink).
class GodotHttpTransport final : public telemetry::ITransport {
public:
	explicit GodotHttpTransport(const godot::String &url);

	telemetry::ShipResult ship(const std::string &envelope) override;
	bool has_endpoint() const override { return valid_; }

private:
	static void post_task(godot::String host, int port, bool tls,
	                      godot::String path, godot::String body);
	void parse_url(const godot::String &url);

	godot::String url_;
	godot::String host_;
	godot::String path_ = "/";
	int  port_  = 0;
	bool tls_   = false;
	bool valid_ = false;
};

} // namespace meridian

#endif // MERIDIAN_GODOT_HTTP_TRANSPORT_H
