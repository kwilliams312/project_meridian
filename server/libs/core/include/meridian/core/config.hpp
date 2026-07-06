// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — layered configuration (stub).
//
// Clean-room: designed from the server SAD (config-driven subsystems — e.g. §2.1
// "protocol version and client_build floor from config", §2.3 OPS-01 tunables)
// and PRD §6 category log levels. No GPL source consulted. See CONTRIBUTING.md.
//
// This is an M0 STUB expressing the *idea* of layered config: defaults are
// overlaid by later layers (file, then environment, then CLI flags), last write
// wins. The real implementation will parse TOML/YAML + env + flags and validate
// against typed schemas per subsystem. The Config surface below is deliberately
// tiny — string key/value with typed getters — so callers can be written now and
// the backend upgraded without touching call sites.

#ifndef MERIDIAN_CORE_CONFIG_HPP
#define MERIDIAN_CORE_CONFIG_HPP

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace meridian::core {

// Precedence of a config source. Higher ordinals override lower ones, modelling
// the layered-config idea (defaults < file < environment < command line).
enum class ConfigLayer : int {
    Default = 0,
    File = 1,
    Environment = 2,
    CommandLine = 3,
};

class Config {
public:
    Config() = default;

    // Set a value, tagged with the layer it came from. A write from an equal or
    // higher layer overrides an existing value; a lower-layer write is ignored.
    // This is the "last (highest) layer wins" rule the real loader will apply as
    // it overlays sources in order.
    void set(std::string_view key, std::string_view value,
             ConfigLayer layer = ConfigLayer::Default);

    // Typed lookups. Return nullopt when the key is absent (or unparseable, for
    // the typed variants).
    std::optional<std::string> get_string(std::string_view key) const;
    std::optional<long long> get_int(std::string_view key) const;
    std::optional<bool> get_bool(std::string_view key) const;

    // Value-or-default helpers for ergonomic call sites.
    std::string get_string_or(std::string_view key, std::string_view fallback) const;
    long long get_int_or(std::string_view key, long long fallback) const;
    bool get_bool_or(std::string_view key, bool fallback) const;

    bool contains(std::string_view key) const;
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::string value;
        ConfigLayer layer;
    };
    std::map<std::string, Entry, std::less<>> entries_;
};

}  // namespace meridian::core

#endif  // MERIDIAN_CORE_CONFIG_HPP
