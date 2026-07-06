// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — minimal leveled logger implementation (M0 stub).
// Real backend: spdlog JSON per PRD §6. Clean-room per CONTRIBUTING.md.

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

// Serialise writes so records from multiple threads never interleave mid-line.
// The real logger owns its own async queue; this mutex is fine for the skeleton.
std::mutex g_write_mutex;

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

// ISO-8601 UTC timestamp, second precision. A stand-in for the JSON logger's
// structured @timestamp field.
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

void set_level(Level level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level get_level() {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void write(Level level, std::string_view category, std::string_view message) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) {
        return;
    }
    // Structured-ish single line: "<ts> <LEVEL> [category] message".
    // The real logger emits the same fields as JSON keys.
    std::string line;
    line.reserve(category.size() + message.size() + 48);
    line += utc_timestamp();
    line += ' ';
    line += level_name(level);
    line += " [";
    line.append(category.data(), category.size());
    line += "] ";
    line.append(message.data(), message.size());
    line += '\n';

    std::lock_guard<std::mutex> guard(g_write_mutex);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

}  // namespace meridian::core::log
