// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — session-flow span catalog: the cross-process grant→trace
// stitching. See include/meridian/trace/session_flow.h for the design.

#include "meridian/trace/session_flow.h"

namespace meridian::trace::flow {

namespace {

// SplitMix64 — a well-known, tiny, high-quality 64-bit mixing function (public
// domain reference by Sebastiano Vigna). We use it ONLY to spread a 64-bit
// grant_id deterministically across the 128-bit trace_id and the 64-bit span_id
// so the authd + worldd hops for the same login derive the SAME ids without a
// wire change. This is NOT security-sensitive (the grant_id is already random +
// non-enumerable from authd; the derived ids are correlation handles, not
// secrets), so a fast deterministic mix is exactly right — and it is fully
// clean-room (SplitMix64 is public-domain, not from any GPL game-server source).
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

template <std::size_t N>
void put_u64(std::array<std::uint8_t, N>& id, std::size_t offset, std::uint64_t v) {
    for (std::size_t i = 0; i < 8 && offset + i < N; ++i) {
        id[offset + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

}  // namespace

SpanContext trace_context_from_grant(std::uint64_t grant_id, bool sampled) {
    SpanContext ctx;
    ctx.sampled = sampled;

    // trace_id (128 bits): two independent SplitMix64 draws off the grant_id, with
    // distinct salts so the two halves differ.
    const std::uint64_t hi = splitmix64(grant_id ^ 0x7472616365000001ULL /*"trace\0\0\x01"*/);
    const std::uint64_t lo = splitmix64(grant_id ^ 0x7472616365000002ULL);
    put_u64(ctx.trace_id, 0, hi);
    put_u64(ctx.trace_id, 8, lo);

    // parent span_id (64 bits): the notional authd "root" span the worldd hop
    // parents onto — also derived from the grant so both sides agree. authd stamps
    // its OWN login span with THIS span_id (so its span IS the parent worldd points
    // at), and worldd calls start_child(...) against this context.
    const std::uint64_t sid = splitmix64(grant_id ^ 0x7370616E00000001ULL /*"span\0\0\0\x01"*/);
    put_u64(ctx.span_id, 0, sid);

    // Guard against the astronomically-unlikely all-zero id (means "absent").
    if (is_zero(ctx.trace_id)) ctx.trace_id[0] = 0x01;
    if (is_zero(ctx.span_id)) ctx.span_id[0] = 0x01;
    return ctx;
}

}  // namespace meridian::trace::flow
