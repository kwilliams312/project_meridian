// Project Meridian — telemetry log core: severity mapping + Sentry-compatible
// JSON envelope serialization (issue #168). Engine-free, zero third-party deps
// (same discipline as the movement core) — a minimal hand-rolled JSON writer.

#include "telemetry_log_core.h"

#include <cstdio>
#include <string>

namespace meridian::telemetry {

const char *sentry_level(Severity s) {
	switch (s) {
		case Severity::Critical: return "fatal";   // Sentry's highest level
		case Severity::Error:    return "error";
		case Severity::Warn:     return "warning";
		case Severity::Info:     return "info";
		case Severity::Debug:    return "debug";
		case Severity::Trace:    return "debug";    // Sentry has no trace
	}
	return "error";
}

namespace {

// JSON-escape a string per RFC 8259: quotes, backslash, control chars. Keeps the
// envelope well-formed even if a log message contains quotes/newlines/etc.
void append_json_escaped(std::string &out, const std::string &in) {
	out.push_back('"');
	for (char c : in) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b";  break;
			case '\f': out += "\\f";  break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x",
					              static_cast<unsigned>(static_cast<unsigned char>(c)));
					out += buf;
				} else {
					out.push_back(c);
				}
		}
	}
	out.push_back('"');
}

void append_kv_string(std::string &out, const char *key, const std::string &value) {
	append_json_escaped(out, key);
	out.push_back(':');
	append_json_escaped(out, value);
}

// The "tags" object carrying ONLY the no-PII context (privacy §2a): session_id,
// build, platform. This is the structural no-PII guarantee — there is no code
// path here that can emit a username/email/ip/hardware field, because
// SessionContext has no such member.
void append_context_tags(std::string &out, const SessionContext &ctx) {
	out += "\"tags\":{";
	append_kv_string(out, "session_id", ctx.session_id);
	out.push_back(',');
	append_kv_string(out, "build", ctx.build);
	out.push_back(',');
	append_kv_string(out, "platform", ctx.platform);
	out.push_back('}');
}

} // namespace

std::string serialize_envelope(const LogBatch &batch) {
	std::string out;
	out.reserve(256 + batch.events.size() * 128);

	// ── Envelope header (one line) ──
	// Sentry envelope: the header carries the sdk + a batch-level event_id-free
	// marker. We keep it minimal and Sentry-parseable.
	out += "{\"sdk\":{\"name\":\"meridian.client.telemetry\",\"version\":\"1\"}}\n";

	// ── One "event" item per captured log event ──
	for (const LogEvent &ev : batch.events) {
		// Item header (declares the item type + payload content-type).
		out += "{\"type\":\"event\",\"content_type\":\"application/json\"}\n";

		// Item payload: a Sentry event. level + message + timestamp + logger +
		// tags(context). NO PII anywhere — only the three permitted context tags.
		out.push_back('{');
		append_kv_string(out, "level", sentry_level(ev.severity));
		out.push_back(',');
		append_kv_string(out, "message", ev.message);
		out.push_back(',');
		append_json_escaped(out, "timestamp");
		out.push_back(':');
		out += std::to_string(ev.timestamp_ms);
		out.push_back(',');
		append_kv_string(out, "logger", ev.logger);
		out.push_back(',');
		append_context_tags(out, batch.context);
		out += "}\n";
	}

	// ── Rate-limit drop summary as a synthetic event (only if any were dropped)
	// so the ingest side can SEE storm suppression without the storm (D-29 rule
	// 2). Still carries only the no-PII context tags.
	if (batch.dropped_by_rate_limit > 0) {
		out += "{\"type\":\"event\",\"content_type\":\"application/json\"}\n";
		out.push_back('{');
		append_kv_string(out, "level", "warning");
		out.push_back(',');
		append_kv_string(out, "message",
		                 "meridian.telemetry: " +
		                     std::to_string(batch.dropped_by_rate_limit) +
		                     " ERROR/CRITICAL events dropped by client rate limit");
		out.push_back(',');
		append_json_escaped(out, "rate_limited_dropped");
		out.push_back(':');
		out += std::to_string(batch.dropped_by_rate_limit);
		out.push_back(',');
		append_context_tags(out, batch.context);
		out += "}\n";
	}

	return out;
}

} // namespace meridian::telemetry
