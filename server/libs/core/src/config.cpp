// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — layered configuration store implementation.
// The four-source loader that fills it lives in config_loader.cpp (issue #90).
// Clean-room per CONTRIBUTING.md.

#include "meridian/core/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace meridian::core {

void Config::set(std::string_view key, std::string_view value, ConfigLayer layer) {
    std::string k(key);
    auto it = entries_.find(k);
    if (it == entries_.end()) {
        entries_.emplace(std::move(k), Entry{std::string(value), layer});
        return;
    }
    // Higher (or equal) layer overrides; a lower-precedence source cannot clobber
    // a value already set by a stronger one. This is the layered-overlay rule.
    if (static_cast<int>(layer) >= static_cast<int>(it->second.layer)) {
        it->second.value = std::string(value);
        it->second.layer = layer;
    }
}

std::optional<std::string> Config::get_string(std::string_view key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    return it->second.value;
}

std::optional<long long> Config::get_int(std::string_view key) const {
    auto raw = get_string(key);
    if (!raw) return std::nullopt;
    try {
        std::size_t pos = 0;
        long long v = std::stoll(*raw, &pos);
        if (pos != raw->size()) return std::nullopt;  // trailing garbage
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> Config::get_bool(std::string_view key) const {
    auto raw = get_string(key);
    if (!raw) return std::nullopt;
    std::string v = *raw;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return std::nullopt;
}

std::string Config::get_string_or(std::string_view key, std::string_view fallback) const {
    return get_string(key).value_or(std::string(fallback));
}

long long Config::get_int_or(std::string_view key, long long fallback) const {
    return get_int(key).value_or(fallback);
}

bool Config::get_bool_or(std::string_view key, bool fallback) const {
    return get_bool(key).value_or(fallback);
}

bool Config::contains(std::string_view key) const {
    return entries_.find(key) != entries_.end();
}

}  // namespace meridian::core
