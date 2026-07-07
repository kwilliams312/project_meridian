// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — Tracer: id generation, sampling, span creation.
// See include/meridian/trace/tracer.h for the design + clean-room statement.

#include "meridian/trace/tracer.h"

#include <algorithm>
#include <random>

namespace meridian::trace {

namespace {

// A thread-local 64-bit PRNG for id bytes. Seeded once per thread from
// std::random_device (+ a per-thread mixing constant so two threads that draw the
// same device value still diverge). Ids are correlation handles — unguessable-
// enough + collision-free is the bar, not cryptographic strength.
std::mt19937_64& thread_rng() {
    thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        // Mix in the thread-local storage address for extra per-thread divergence.
        seed ^= reinterpret_cast<std::uintptr_t>(&seed) * 0x9E3779B97F4A7C15ULL;
        return seed;
    }());
    return rng;
}

template <std::size_t N>
void fill_random_bytes(std::array<std::uint8_t, N>& id) {
    auto& rng = thread_rng();
    std::size_t i = 0;
    while (i < N) {
        std::uint64_t r = rng();
        for (int b = 0; b < 8 && i < N; ++b, ++i) {
            id[i] = static_cast<std::uint8_t>((r >> (8 * b)) & 0xFF);
        }
    }
    // A valid id must be non-zero (an all-zero id means "absent"). The odds are
    // 2^-128 / 2^-64, but force it so a span is never accidentally "invalid".
    bool any = false;
    for (std::uint8_t x : id) {
        if (x != 0) { any = true; break; }
    }
    if (!any) id[0] = 0x01;
}

// A 53-bit draw in [0,1) for the ratio sampler (double has 53 bits of mantissa).
double unit_random() {
    auto& rng = thread_rng();
    // Top 53 bits / 2^53 — an unbiased double in [0,1).
    std::uint64_t r = rng() >> 11;
    return static_cast<double>(r) / static_cast<double>(1ULL << 53);
}

}  // namespace

void fill_random(TraceId& id) { fill_random_bytes(id); }
void fill_random(SpanId& id) { fill_random_bytes(id); }

Tracer::Tracer(std::string service_name, std::string realm)
    : service_name_(std::move(service_name)), realm_(std::move(realm)) {}

void Tracer::set_sample_ratio(double ratio) {
    sample_ratio_.store(std::clamp(ratio, 0.0, 1.0), std::memory_order_relaxed);
}
double Tracer::sample_ratio() const {
    return sample_ratio_.load(std::memory_order_relaxed);
}

bool Tracer::should_sample() const {
    double r = sample_ratio_.load(std::memory_order_relaxed);
    if (r >= 1.0) return true;   // sample all (M0 default)
    if (r <= 0.0) return false;  // sample none
    return unit_random() < r;
}

Span Tracer::start_span(const std::string& name, SpanKind kind) {
    Span s;
    fill_random(s.trace_id);
    fill_random(s.span_id);
    // parent_span_id stays all-zero (root).
    s.name = name;
    s.kind = kind;
    s.start_unix_nano = now_unix_nano();
    return s;
}

Span Tracer::start_child(const std::string& name, const SpanContext& parent,
                         SpanKind kind) {
    if (!parent.valid()) {
        // No usable upstream context — degrade to a fresh root rather than emit a
        // parentless/broken span.
        return start_span(name, kind);
    }
    Span s;
    s.trace_id = parent.trace_id;   // same trace
    fill_random(s.span_id);         // fresh child id
    s.parent_span_id = parent.span_id;
    s.name = name;
    s.kind = kind;
    s.start_unix_nano = now_unix_nano();
    return s;
}

SpanContext Tracer::context_of(const Span& span, bool sampled) {
    SpanContext ctx;
    ctx.trace_id = span.trace_id;
    ctx.span_id = span.span_id;
    ctx.sampled = sampled;
    return ctx;
}

}  // namespace meridian::trace
