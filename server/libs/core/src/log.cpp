// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — structured leveled logger implementation (OPS-05, #165).
//
// Renders each record as either a Loki-ingestable JSON object (stdout, prod) or
// a human-readable text line (stderr, dev). The JSON shape matches telemetryd's
// #167 client-ingest sink so server-daemon logs and client-ingest logs share one
// schema. Clean-room per CONTRIBUTING.md — no GPL source, no JSON dependency; the
// escaper mirrors the hand-rolled one in telemetryd/ingest.cpp.

#include "meridian/core/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace meridian::core::log {

namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::atomic<int> g_format{static_cast<int>(Format::Json)};

// Serialise writes so records from multiple threads never interleave mid-line.
std::mutex g_write_mutex;

// Process/realm labels stamped on every JSON record. Guarded by g_label_mutex —
// set once at startup, then read on every log call. A mutex (not atomic) because
// std::string isn't trivially atomic; contention is nil (writes are startup-only).
std::mutex g_label_mutex;
std::string g_process = "meridian";
std::string g_realm = "reference";

char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower(a[i]) != to_lower(b[i])) return false;
    }
    return true;
}

// ISO-8601 UTC timestamp, second precision — the text-mode @timestamp.
std::string utc_timestamp() {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

// Milliseconds since the Unix epoch — the JSON `timestamp_ms` (matches the
// telemetryd sink field and telemetry-architecture.md §5.2).
std::uint64_t now_ms() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch())
            .count());
}

// JSON string escaper — same rules as telemetryd/ingest.cpp's json_escape so the
// two sinks produce byte-identical escaping for shared fields.
void json_escape(std::string& out, std::string_view in) {
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

// Emit `"key":` then the value — string (escaped) or bare number.
void json_kv(std::string& out, std::string_view key, std::string_view val,
             bool is_number) {
    json_escape(out, key);
    out.push_back(':');
    if (is_number) {
        out.append(val.data(), val.size());
    } else {
        json_escape(out, val);
    }
}

}  // namespace

Level level_from_string(std::string_view name) {
    if (iequals(name, "trace")) return Level::Trace;
    if (iequals(name, "debug")) return Level::Debug;
    if (iequals(name, "info")) return Level::Info;
    if (iequals(name, "warn") || iequals(name, "warning")) return Level::Warn;
    if (iequals(name, "error")) return Level::Error;
    return Level::Info;
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

const char* level_loki(Level level) {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

Format format_from_string(std::string_view name) {
    if (iequals(name, "text")) return Format::Text;
    if (iequals(name, "json")) return Format::Json;
    return Format::Json;
}

void set_level(Level level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level get_level() {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void set_format(Format format) {
    g_format.store(static_cast<int>(format), std::memory_order_relaxed);
}

Format get_format() {
    return static_cast<Format>(g_format.load(std::memory_order_relaxed));
}

void set_process(std::string_view process) {
    std::lock_guard<std::mutex> guard(g_label_mutex);
    g_process.assign(process.data(), process.size());
}

void set_realm(std::string_view realm) {
    std::lock_guard<std::mutex> guard(g_label_mutex);
    g_realm.assign(realm.data(), realm.size());
}

// --- Rendering --------------------------------------------------------------

std::string render_json(Level level, std::string_view category,
                        std::string_view message, const Fields& fields) {
    std::string process, realm;
    {
        std::lock_guard<std::mutex> guard(g_label_mutex);
        process = g_process;
        realm = g_realm;
    }
    const char* loki = level_loki(level);

    // Loki body shape (telemetry-architecture.md §5.2), field order matching
    // telemetryd/ingest.cpp forward_event_json: realm/process/level/event are the
    // low-cardinality labels; severity/logger/message/timestamp_ms are body. We
    // set `event` to the subsystem `category` — it names the log stream, exactly
    // as telemetryd's `event` names the ingest stream. `logger` carries the same
    // subsystem tag (the JSON analogue of the old text "[category]").
    std::string out;
    out.reserve(160 + message.size() + category.size());
    out.push_back('{');
    json_kv(out, "realm", realm, false);            out.push_back(',');
    json_kv(out, "process", process, false);        out.push_back(',');
    json_kv(out, "level", loki, false);             out.push_back(',');
    json_kv(out, "event", category, false);         out.push_back(',');
    json_kv(out, "severity", loki, false);          out.push_back(',');
    json_kv(out, "logger", category, false);        out.push_back(',');
    json_kv(out, "message", message, false);        out.push_back(',');
    json_kv(out, "timestamp_ms", std::to_string(now_ms()), true);
    for (const Field& f : fields) {
        out.push_back(',');
        json_kv(out, f.key, f.value, f.is_number);
    }
    out.push_back('}');
    return out;
}

std::string render_text(Level level, std::string_view category,
                        std::string_view message, const Fields& fields) {
    // Legacy human-readable line: "<ts> LEVEL [category] message key=value ...".
    std::string line;
    line.reserve(category.size() + message.size() + 48);
    line += utc_timestamp();
    line += ' ';
    line += level_name(level);
    line += " [";
    line.append(category.data(), category.size());
    line += "] ";
    line.append(message.data(), message.size());
    for (const Field& f : fields) {
        line += ' ';
        line += f.key;
        line += '=';
        line += f.value;
    }
    return line;
}

void write(Level level, std::string_view category, std::string_view message,
           const Fields& fields) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) {
        return;
    }
    const Format fmt = get_format();
    std::string line = (fmt == Format::Json)
                           ? render_json(level, category, message, fields)
                           : render_text(level, category, message, fields);
    line.push_back('\n');

    // JSON goes to stdout (the collector tails it, uniform with telemetryd's
    // stdout sink); text goes to stderr (dev readability, keeps stdout clean).
    std::FILE* sink = (fmt == Format::Json) ? stdout : stderr;
    std::lock_guard<std::mutex> guard(g_write_mutex);
    std::fputs(line.c_str(), sink);
    std::fflush(sink);
}

void write(Level level, std::string_view category, std::string_view message) {
    static const Fields kNoFields;
    write(level, category, message, kNoFields);
}

}  // namespace meridian::core::log
