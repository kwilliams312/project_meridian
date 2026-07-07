// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — structured leveled logger (OPS-05, #165).
//
// Clean-room: designed from server PRD §6 ("structured JSON logs with category
// levels"), server SAD §8.5 (observability), docs/telemetry-architecture.md §5.2
// (Loki log-line shape), and D-29 §9 (OPS-05 log pipeline). No GPL source
// consulted. See CONTRIBUTING.md.
//
// This is the OPS-05 log pipeline (#165). Every server daemon (authd, worldd,
// telemetryd) funnels its log call sites through this single choke point, which
// renders each record either as
//
//   * a Loki-ingestable JSON object, one per line, on stdout (prod default), or
//   * a human-readable text line on stderr (`--log-format=text`, dev default via
//     run-local.sh), preserving the legacy "<ts> LEVEL [category] message" shape.
//
// The JSON shape MATCHES telemetryd's #167 client-ingest sink
// (server/telemetryd/ingest.cpp -> forward_event_json) so client-ingest logs and
// server-daemon logs share ONE schema and a collector can tail them uniformly:
//
//   {"realm":"...","process":"worldd","level":"info","event":"...",
//    "severity":"info","logger":"world","message":"...","timestamp_ms":N,
//    <structured fields...>}
//
// Loki labels (low cardinality) are `realm`, `process`, `level`, `event`
// (telemetry-architecture.md §5.2); high-cardinality context (session_id,
// opcode, grant_id) lives in the body as typed `Field`s, queried but not
// indexed. `logger` is the free-form subsystem tag (the old `category`).

#ifndef MERIDIAN_CORE_LOG_HPP
#define MERIDIAN_CORE_LOG_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace meridian::core::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

// Output rendering. Text = legacy human-readable line on stderr (dev);
// Json = one Loki-ingestable JSON object per line on stdout (prod default).
enum class Format : int {
    Json = 0,
    Text = 1,
};

// A typed structured field carried in the JSON body (e.g. session_id, opcode,
// grant_id). `is_number` renders the value as a bare JSON number (no quotes);
// otherwise it is a JSON string. In text mode fields render as key=value pairs.
struct Field {
    std::string key;
    std::string value;
    bool is_number = false;
};

// Field constructors — the ergonomic way to attach typed context to a log call:
//   log::info("world", "session entered", {log::field("slot", slot),
//                                          log::field("guid", guid_str)});
inline Field field(std::string key, std::string value) {
    return Field{std::move(key), std::move(value), /*is_number=*/false};
}
inline Field field(std::string key, const char* value) {
    return Field{std::move(key), std::string(value), /*is_number=*/false};
}
inline Field field(std::string key, std::int64_t value) {
    return Field{std::move(key), std::to_string(value), /*is_number=*/true};
}
inline Field field(std::string key, std::uint64_t value) {
    return Field{std::move(key), std::to_string(value), /*is_number=*/true};
}
inline Field field(std::string key, int value) {
    return Field{std::move(key), std::to_string(value), /*is_number=*/true};
}
inline Field field(std::string key, unsigned value) {
    return Field{std::move(key), std::to_string(value), /*is_number=*/true};
}
inline Field field(std::string key, double value) {
    return Field{std::move(key), std::to_string(value), /*is_number=*/true};
}
inline Field field(std::string key, bool value) {
    return Field{std::move(key), value ? "true" : "false", /*is_number=*/true};
}

using Fields = std::vector<Field>;

// Parse a level name ("trace".."error", case-insensitive). Unknown -> Info.
Level level_from_string(std::string_view name);

// Short uppercase tag for a level ("INFO", "WARN", ...) — text mode.
const char* level_name(Level level);

// Lowercase Loki level string ("info", "warn", "error", ...) — the JSON
// `level`/`severity` value. Matches telemetryd's severity_str() vocabulary.
const char* level_loki(Level level);

// Parse a format name ("json"|"text", case-insensitive). Unknown -> Json.
Format format_from_string(std::string_view name);

// Process-global minimum level. Records below this are dropped. Default: Info.
void set_level(Level level);
Level get_level();

// Process-global output format. Default: Json (prod). run-local.sh / dev sets
// Text for readable local output. Thread-safe (atomic).
void set_format(Format format);
Format get_format();

// The `process` label stamped on every JSON record ("authd"/"worldd"/
// "telemetryd"). Set once at daemon startup. Default: "meridian".
void set_process(std::string_view process);

// The `realm` label stamped on every JSON record — the low-cardinality Loki
// label grouping a daemon's logs (matches the metrics realm label). Default:
// "reference".
void set_realm(std::string_view realm);

// Apply logging config from the environment (the 12-factor override path the
// daemons and compose stack use, before flags): MERIDIAN_LOG_FORMAT
// (json|text) and MERIDIAN_LOG_LEVEL (trace..error). Unset vars leave the
// current value. Called at daemon startup; explicit --log-format/--log-level
// flags override afterwards.
void configure_from_env();

// Emit one record. `category` is the subsystem tag (rendered as the JSON
// `logger` field and the text `[category]` tag). `fields` are typed structured
// key-values placed in the JSON body. Thread-safe: the whole line is written
// under a lock so JSON objects from concurrent threads never interleave.
void write(Level level, std::string_view category, std::string_view message,
           const Fields& fields);

// Field-less overload (legacy call sites): renders with no structured body.
void write(Level level, std::string_view category, std::string_view message);

// Convenience wrappers — both the legacy (no fields) and structured forms.
inline void trace(std::string_view category, std::string_view message) {
    write(Level::Trace, category, message);
}
inline void debug(std::string_view category, std::string_view message) {
    write(Level::Debug, category, message);
}
inline void info(std::string_view category, std::string_view message) {
    write(Level::Info, category, message);
}
inline void warn(std::string_view category, std::string_view message) {
    write(Level::Warn, category, message);
}
inline void error(std::string_view category, std::string_view message) {
    write(Level::Error, category, message);
}

inline void trace(std::string_view category, std::string_view message, Fields fields) {
    write(Level::Trace, category, message, fields);
}
inline void debug(std::string_view category, std::string_view message, Fields fields) {
    write(Level::Debug, category, message, fields);
}
inline void info(std::string_view category, std::string_view message, Fields fields) {
    write(Level::Info, category, message, fields);
}
inline void warn(std::string_view category, std::string_view message, Fields fields) {
    write(Level::Warn, category, message, fields);
}
inline void error(std::string_view category, std::string_view message, Fields fields) {
    write(Level::Error, category, message, fields);
}

// --- Testing seam -----------------------------------------------------------
// Render a single record to a string WITHOUT touching stdout/stderr or the
// write lock. `render_json` always emits JSON regardless of the global format;
// `render_text` always emits the text line. Used by the unit test to parse a
// record back and assert its fields deterministically. Not part of the hot path.
std::string render_json(Level level, std::string_view category,
                        std::string_view message, const Fields& fields);
std::string render_text(Level level, std::string_view category,
                        std::string_view message, const Fields& fields);

}  // namespace meridian::core::log

#endif  // MERIDIAN_CORE_LOG_HPP
