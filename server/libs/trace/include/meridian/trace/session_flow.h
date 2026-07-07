// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — the session-flow span catalog (OPS-05; D-29 §9 rule 5;
// docs/telemetry-architecture.md §5.3).
//
// This is the ONE place the session-flow span NAMES + attribute KEYS are declared,
// so the two instrumented daemons emit a consistent, greppable trace (the same
// discipline as metrics/catalog.h for metric names). The session-flow trace the
// signal catalog specifies is:
//
//   auth → grant  (authd span "authd.login")   ──┐  linked by the grant_id
//   world handshake → enter-world               ─┘  (D-29 §9 rule 5: the grant
//   (worldd span "worldd.enter_world")               relates the two hops)
//
// At M0 authd and worldd are separate processes and the client carries the grant
// between them, so the two spans are emitted independently but SHARE a trace_id
// derived from the grant (see trace_context_from_grant): they stitch into one
// end-to-end session-establishment trace in the backend without a wire-protocol
// change. When the M2 gateway split lands, the bus envelope's reserved trace-
// context field (D-29 §9 rule 5) carries the real parent span_id and the derived
// stitching is replaced by true parent/child propagation — no span-name change.

#ifndef MERIDIAN_TRACE_SESSION_FLOW_H
#define MERIDIAN_TRACE_SESSION_FLOW_H

#include <cstdint>

#include "meridian/trace/tracer.h"

namespace meridian::trace::flow {

// Span names (operation identifiers on the OTLP wire).
inline constexpr const char* kAuthdLogin = "authd.login";           // authd: SRP verify → grant issue
inline constexpr const char* kWorlddEnterWorld = "worldd.enter_world";  // worldd: grant validate → HandshakeOk

// Attribute keys (kept stable + low-cardinality; no PII per telemetry-privacy §3).
inline constexpr const char* kAttrRealm = "meridian.realm";
inline constexpr const char* kAttrOutcome = "meridian.auth.outcome";      // authd login outcome label
inline constexpr const char* kAttrGrantIssued = "meridian.auth.grant_issued";
inline constexpr const char* kAttrRealmId = "meridian.realm_id";          // worldd: grant's realm binding
inline constexpr const char* kAttrHandshake = "meridian.world.handshake_ok";
inline constexpr const char* kAttrGrantReject = "meridian.world.grant_reject";  // reject classification (server-side)

// Derive a STABLE trace context from a session grant_id so the authd span and the
// worldd span for the SAME login stitch into one trace WITHOUT a wire change (D-29
// §9 rule 5). Both daemons call this with the same grant_id and get the same
// trace_id; each mints its own span_id. The authd span is the notional root, so
// worldd parents its span onto a span_id ALSO derived deterministically from the
// grant (the "authd root span" placeholder) — giving a well-formed parent/child
// pair across the process boundary at M0. `sampled` mirrors the M0 sample-all
// default; a daemon may override with its Tracer's ratio.
//
// The derivation is a simple, documented mixing of the 64-bit grant_id into the
// 128-bit trace_id + 64-bit span_id (NOT cryptographic — these are correlation
// handles, and the grant_id is already a random, non-enumerable u64 from authd).
SpanContext trace_context_from_grant(std::uint64_t grant_id, bool sampled = true);

}  // namespace meridian::trace::flow

#endif  // MERIDIAN_TRACE_SESSION_FLOW_H
