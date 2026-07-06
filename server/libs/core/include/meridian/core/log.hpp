// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — minimal leveled logger.
//
// Clean-room: designed from server PRD §6 ("structured JSON logs (spdlog) with
// category levels; separate append-only audit streams"). No GPL source
// consulted. See CONTRIBUTING.md.
//
// This is an M0 STUB. It writes structured-ish single-line records to stderr so
// the daemons have observable output today. The real logger (PRD §6, tracked in
// server SAD §7 "Metrics/logs: stub + tick histogram") is spdlog emitting JSON
// with per-category levels plus separate audit streams (GM/economy/anti-cheat).
// The public surface here — Level, set_level, category, the log macros — is
// shaped so that swap is a backend change, not an API change.

#ifndef MERIDIAN_CORE_LOG_HPP
#define MERIDIAN_CORE_LOG_HPP

#include <string_view>

namespace meridian::core::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

// Parse a level name ("trace".."error", case-insensitive). Unknown -> Info.
Level level_from_string(std::string_view name);

// Short uppercase tag for a level ("INFO", "WARN", ...).
const char* level_name(Level level);

// Process-global minimum level. Records below this are dropped. Default: Info.
// Not yet thread-synchronised beyond the atomicity of a single int store; the
// real logger owns its own threading (PRD §6). Adequate for the M0 skeleton.
void set_level(Level level);
Level get_level();

// Emit one record. `category` is a free-form subsystem tag (e.g. "authd",
// "worldd", "net", "db") — the seam for the JSON logger's category levels and
// for D-23 realm/zone/shard log context. `message` is the payload.
// Thread-safe at the granularity of one write(2) to stderr.
void write(Level level, std::string_view category, std::string_view message);

// Convenience wrappers.
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

}  // namespace meridian::core::log

#endif  // MERIDIAN_CORE_LOG_HPP
