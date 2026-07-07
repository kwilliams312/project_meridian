// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — layered configuration (stub).
//
// Clean-room: designed from the server SAD (config-driven subsystems — e.g. §2.1
// "protocol version and client_build floor from config", §2.3 OPS-01 tunables)
// and PRD §6 category log levels. No GPL source consulted. See CONTRIBUTING.md.
//
// This header is the typed key/value STORE at the heart of the layered config:
// defaults are overlaid by later layers (file, then environment, then CLI flags),
// with the highest layer winning (see the ConfigLayer precedence rule below).
// The Config surface is deliberately tiny — string key/value with typed getters —
// so call sites stay stable.
//
// The layered LOADER that fills a Config from the four sources (a TOML/INI-subset
// file, the MERIDIAN_* environment, and "--key=value" flags) lives in the
// companion header config_loader.hpp (issue #90). Typed per-subsystem schema
// validation remains a later concern; today the daemons read typed values with
// documented defaults (see docs/ops/server-config.md for the key catalog).

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
