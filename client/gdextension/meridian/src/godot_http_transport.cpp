// Project Meridian — Godot HTTP telemetry transport (issues #168, #109).
// Extracted from meridian_telemetry.cpp so the #168 log channel and the #109
// crash channel share ONE transport. See godot_http_transport.h.

#include "godot_http_transport.h"

#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/tls_options.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;

namespace meridian {

GodotHttpTransport::GodotHttpTransport(const String &url) : url_(url) {
	// Split the URL into host / port / path / tls once. A malformed URL leaves
	// has_endpoint() false so the pipeline treats it as no-sink.
	parse_url(url);
}

telemetry::ShipResult GodotHttpTransport::ship(const std::string &envelope) {
	if (!valid_) {
		return telemetry::ShipResult::NoSink;
	}
	// Enqueue off-thread so the game loop never blocks on network IO. Copy the
	// envelope + connection params into the task; fire-and-forget.
	String host = host_;
	int port = port_;
	bool tls = tls_;
	String path = path_;
	String body = String::utf8(envelope.c_str());

	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (pool == nullptr) {
		return telemetry::ShipResult::Failed;
	}
	Callable task = callable_mp_static(&GodotHttpTransport::post_task)
	                    .bind(host, port, tls, path, body);
	pool->add_task(task, /*high_priority=*/false, "meridian.telemetry.ship");
	return telemetry::ShipResult::Ok;
}

void GodotHttpTransport::post_task(String host, int port, bool tls, String path, String body) {
	Ref<HTTPClient> http = memnew(HTTPClient);
	Error err = http->connect_to_host(host, port,
	                                  tls ? Ref<TLSOptions>() : Ref<TLSOptions>());
	if (err != OK) {
		return;
	}
	// Poll until connected (bounded).
	for (int i = 0; i < 1000; ++i) {
		http->poll();
		HTTPClient::Status st = http->get_status();
		if (st == HTTPClient::STATUS_CONNECTED) break;
		if (st == HTTPClient::STATUS_CANT_CONNECT ||
		    st == HTTPClient::STATUS_CANT_RESOLVE ||
		    st == HTTPClient::STATUS_CONNECTION_ERROR) {
			return;
		}
	}
	if (http->get_status() != HTTPClient::STATUS_CONNECTED) {
		return;
	}

	PackedStringArray headers;
	headers.push_back("Content-Type: application/x-sentry-envelope");
	headers.push_back("Accept: application/json");
	http->request(HTTPClient::METHOD_POST, path, headers, body);

	// Drive the request to completion (bounded) — fire-and-forget.
	for (int i = 0; i < 2000; ++i) {
		http->poll();
		HTTPClient::Status st = http->get_status();
		if (st == HTTPClient::STATUS_BODY || st == HTTPClient::STATUS_CONNECTED) break;
		if (st == HTTPClient::STATUS_DISCONNECTED ||
		    st == HTTPClient::STATUS_CONNECTION_ERROR) {
			return;
		}
	}
	http->close();
}

void GodotHttpTransport::parse_url(const String &url) {
	// Accept http(s)://host[:port]/path. Minimal parse — the endpoint is a
	// project-config value, not user input.
	String u = url.strip_edges();
	if (u.is_empty()) { valid_ = false; return; }
	if (u.begins_with("https://")) { tls_ = true; u = u.substr(8); }
	else if (u.begins_with("http://")) { tls_ = false; u = u.substr(7); }
	else { valid_ = false; return; }

	int slash = u.find("/");
	String authority = slash < 0 ? u : u.substr(0, slash);
	path_ = slash < 0 ? String("/") : u.substr(slash);

	int colon = authority.find(":");
	if (colon < 0) {
		host_ = authority;
		port_ = tls_ ? 443 : 80;
	} else {
		host_ = authority.substr(0, colon);
		port_ = authority.substr(colon + 1).to_int();
	}
	valid_ = !host_.is_empty();
}

} // namespace meridian
