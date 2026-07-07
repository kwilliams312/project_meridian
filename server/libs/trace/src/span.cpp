// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — span model helpers (ids, attributes, timestamps).
// See include/meridian/trace/span.h for the design + clean-room statement.

#include "meridian/trace/span.h"

#include <chrono>

namespace meridian::trace {

namespace {

constexpr char kHex[] = "0123456789abcdef";

template <std::size_t N>
std::string hex_of(const std::array<std::uint8_t, N>& id) {
    std::string out;
    out.reserve(N * 2);
    for (std::uint8_t b : id) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// Parse one hex nibble; returns -1 on a non-hex char.
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <std::size_t N>
bool from_hex(const std::string& hex, std::array<std::uint8_t, N>& out) {
    if (hex.size() != N * 2) return false;
    std::array<std::uint8_t, N> tmp{};
    for (std::size_t i = 0; i < N; ++i) {
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        tmp[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    out = tmp;
    return true;
}

template <std::size_t N>
bool all_zero(const std::array<std::uint8_t, N>& id) {
    for (std::uint8_t b : id) {
        if (b != 0) return false;
    }
    return true;
}

}  // namespace

std::string to_hex(const TraceId& id) { return hex_of(id); }
std::string to_hex(const SpanId& id) { return hex_of(id); }

bool trace_id_from_hex(const std::string& hex, TraceId& out) { return from_hex(hex, out); }
bool span_id_from_hex(const std::string& hex, SpanId& out) { return from_hex(hex, out); }

bool is_zero(const TraceId& id) { return all_zero(id); }
bool is_zero(const SpanId& id) { return all_zero(id); }

Attribute attr(std::string key, std::string value) {
    Attribute a;
    a.key = std::move(key);
    a.type = Attribute::Type::kString;
    a.str_value = std::move(value);
    return a;
}
Attribute attr(std::string key, const char* value) {
    return attr(std::move(key), std::string(value ? value : ""));
}
Attribute attr(std::string key, std::int64_t value) {
    Attribute a;
    a.key = std::move(key);
    a.type = Attribute::Type::kInt;
    a.int_value = value;
    return a;
}
Attribute attr(std::string key, int value) {
    return attr(std::move(key), static_cast<std::int64_t>(value));
}
Attribute attr(std::string key, bool value) {
    Attribute a;
    a.key = std::move(key);
    a.type = Attribute::Type::kBool;
    a.bool_value = value;
    return a;
}
Attribute attr(std::string key, double value) {
    Attribute a;
    a.key = std::move(key);
    a.type = Attribute::Type::kDouble;
    a.double_value = value;
    return a;
}

std::uint64_t now_unix_nano() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

Span& Span::end() {
    if (end_unix_nano == 0) end_unix_nano = now_unix_nano();
    return *this;
}

}  // namespace meridian::trace
