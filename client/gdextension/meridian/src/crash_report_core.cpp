// Project Meridian — client crash report core (issue #109). Engine-free, zero
// third-party deps (same discipline as telemetry_log_core.*): a minimal
// hand-rolled report-file (de)serialiser + a Sentry-compatible envelope writer
// that matches the #168 writer byte-for-byte in shape so the #167 ingest parses
// a crash exactly like a log event.

#include "crash_report_core.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace meridian::crash {

const char *signal_name(int sig) {
	// The POSIX signals a crash handler installs for (Client SAD §5.5 fatal
	// conditions). Kept as a small explicit table so the name is stable regardless
	// of platform <csignal> macro values.
	switch (sig) {
		case 4:  return "SIGILL";
		case 6:  return "SIGABRT";
		case 8:  return "SIGFPE";
		case 10: return "SIGBUS";   // BSD/macOS value; Linux SIGBUS=7 handled below
		case 11: return "SIGSEGV";
		case 7:  return "SIGBUS";   // Linux SIGBUS
		default: return "SIG?";
	}
}

std::string crash_summary(const CrashReport &r) {
	std::string out = signal_name(r.signal_number);
	out += " (signal ";
	out += std::to_string(r.signal_number);
	out += ") at 0x";
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(r.fault_address));
	out += buf;
	out += " — ";
	out += std::to_string(r.frames.size());
	out += " frames";
	return out;
}

// ===========================================================================
// On-disk report file (v1).
// ===========================================================================
namespace {

constexpr const char *kMagic = "MERIDIAN-CRASH v1";

void append_hex_line(std::string &out, const char *key, uint64_t v) {
	out += key;
	out.push_back(' ');
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
	out += buf;
	out.push_back('\n');
}

void append_dec_line(std::string &out, const char *key, uint64_t v) {
	out += key;
	out.push_back(' ');
	out += std::to_string(v);
	out.push_back('\n');
}

void append_str_line(std::string &out, const char *key, const std::string &v) {
	out += key;
	out.push_back(' ');
	out += v;   // verbatim to EOL (controlled token, no newline)
	out.push_back('\n');
}

// Parse an unsigned hex value from a token (no 0x prefix).
uint64_t parse_hex(const std::string &tok) {
	return static_cast<uint64_t>(std::strtoull(tok.c_str(), nullptr, 16));
}

uint64_t parse_dec(const std::string &tok) {
	return static_cast<uint64_t>(std::strtoull(tok.c_str(), nullptr, 10));
}

} // namespace

std::string serialize_report_file(const CrashReport &r) {
	std::string out;
	out.reserve(128 + r.frames.size() * 20);
	out += kMagic;
	out.push_back('\n');
	append_dec_line(out, "sig", static_cast<uint64_t>(r.signal_number));
	append_hex_line(out, "addr", r.fault_address);
	append_dec_line(out, "time", r.timestamp_ms);
	append_str_line(out, "sid", r.context.session_id);
	append_str_line(out, "build", r.context.build);
	append_str_line(out, "plat", r.context.platform);
	for (uint64_t f : r.frames) {
		append_hex_line(out, "fr", f);
	}
	return out;
}

bool parse_report_file(const std::string &text, CrashReport &out) {
	out = CrashReport{};
	// Split into lines.
	std::size_t pos = 0;
	bool first = true;
	while (pos <= text.size()) {
		std::size_t nl = text.find('\n', pos);
		std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
		pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

		// Trim a trailing '\r' (tolerate CRLF).
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (first) {
			if (line != kMagic) return false;   // not a v1 report
			first = false;
			continue;
		}
		if (line.empty()) continue;

		// Split "key value" on the FIRST space; value is the remainder (may be empty).
		std::size_t sp = line.find(' ');
		std::string key = (sp == std::string::npos) ? line : line.substr(0, sp);
		std::string val = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);

		if (key == "sig")        out.signal_number = static_cast<int>(parse_dec(val));
		else if (key == "addr")  out.fault_address = parse_hex(val);
		else if (key == "time")  out.timestamp_ms  = parse_dec(val);
		else if (key == "sid")   out.context.session_id = val;
		else if (key == "build") out.context.build      = val;
		else if (key == "plat")  out.context.platform   = val;
		else if (key == "fr")    out.frames.push_back(parse_hex(val));
		// unknown keys ignored (forward-compat)
	}
	if (first) return false;   // empty input: never saw the magic
	out.signal_name = signal_name(out.signal_number);
	return true;
}

// ===========================================================================
// Sentry-compatible envelope (matches the #168 writer shape).
// ===========================================================================
namespace {

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

// The no-PII context tags (privacy §2a) — identical shape to the #168 writer, so
// the ingest tag allow-list (session_id/build/platform ONLY) accepts it verbatim.
void append_context_tags(std::string &out, const CrashContext &ctx) {
	out += "\"tags\":{";
	append_kv_string(out, "session_id", ctx.session_id);
	out.push_back(',');
	append_kv_string(out, "build", ctx.build);
	out.push_back(',');
	append_kv_string(out, "platform", ctx.platform);
	out.push_back('}');
}

// A hex "0x.." address string for the frames array.
std::string hex_addr(uint64_t v) {
	char buf[24];
	std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
	return buf;
}

} // namespace

std::string serialize_crash_envelope(const CrashReport &r) {
	std::string out;
	out.reserve(256 + r.frames.size() * 24);

	// ── Envelope header (identical to the #168 writer). ──
	out += "{\"sdk\":{\"name\":\"meridian.client.telemetry\",\"version\":\"1\"}}\n";

	// ── One "event" item carrying the crash as a Sentry fatal event. ──
	out += "{\"type\":\"event\",\"content_type\":\"application/json\"}\n";
	out.push_back('{');
	append_kv_string(out, "level", "fatal");   // Sentry's highest (Critical maps here)
	out.push_back(',');
	append_kv_string(out, "message", crash_summary(r));
	out.push_back(',');
	append_json_escaped(out, "timestamp");
	out.push_back(':');
	out += std::to_string(r.timestamp_ms);
	out.push_back(',');
	append_kv_string(out, "logger", "crash");
	out.push_back(',');
	// The marker the ingest keys on to count meridian_client_crash_total. Top-level
	// payload field (NOT a tag) so it passes the ingest tag allow-list, and not a
	// PII-shaped key so it passes the defense-in-depth PII scan.
	append_kv_string(out, "event_kind", "crash");
	out.push_back(',');
	// The backtrace as a single space-joined STRING of hex addresses (symbolised
	// strings when present, else raw addresses). A string — NOT a JSON array — so
	// the #167 ingest's minimal (string/number/object) parser accepts it unchanged;
	// the ingest forwards it verbatim to the sink. Diagnostic only — no PII.
	{
		std::string frames_str;
		for (std::size_t i = 0; i < r.frames.size(); ++i) {
			if (i) frames_str.push_back(' ');
			if (i < r.symbols.size() && !r.symbols[i].empty()) {
				frames_str += r.symbols[i];
			} else {
				frames_str += hex_addr(r.frames[i]);
			}
		}
		append_kv_string(out, "frames", frames_str);
	}
	out.push_back(',');
	append_context_tags(out, r.context);
	out += "}\n";

	return out;
}

} // namespace meridian::crash
