// SPDX-License-Identifier: Apache-2.0
//
// authd — IF-1 login state machine (server SAD §5.1, §3.1, §2.1; issue #79).
//
// Provenance: designed from the server SAD only — §5.1 (the IF-1 message flow:
// ClientHello/ServerHello, SrpStart -> SrpChallenge{salt,B}, SrpProof{A,M1} ->
// AuthResult{M2|error}, RealmListRequest -> RealmList, RealmSelect{realm_id} ->
// SessionGrant{grant_id,session_key}|Error), §3.1 (the login->grant sequence
// diagram and the single-use atomic-consume rule for session_grant), §4.1 (the
// auth DB `account`, `realm` and `session_grant` field lists). The wire contract
// is schema/net/auth.fbs. Composes FIVE merged libs — meridian-net (Session:
// TLS 1.3 + IF-1 framing), meridian-proto (auth.fbs FlatBuffers), meridian-srp
// (ServerSession), meridian-db (Connection). Clean-room, original code: no GPL
// source (CMaNGOS / TrinityCore or otherwise) consulted (CONTRIBUTING.md).
//
// This is the testable core: `run_login` drives ONE client session end-to-end
// over an already-accepted meridian::net::Session. It is deliberately NOT buried
// in main() so the integration test can point it at the real TLS listener and
// assert the full flow (SAD §5.1) plus the DB side effects (a single-use grant).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"
#include "meridian/srp/srp.h"
#include "meridian/trace/exporter.h"

namespace meridian::authd {

// Server-side login policy + advertised identity (SAD §5.1). Loaded by main()
// from flags/env; the integration test constructs it directly.
struct LoginConfig {
    // The build/proto authd advertises back in ServerHello.
    std::uint32_t server_build = 1;
    std::uint16_t proto_ver = 1;

    // ClientHello gate (SAD §2.1 "build floor"): a ClientHello whose build is
    // below this floor, or whose proto_ver mismatches `proto_ver`, is rejected
    // before any SRP work. 0 = no floor.
    std::uint32_t build_floor = 0;

    // SRP-6a parameters. Must match meridian-account's defaults (2048-bit group
    // + SHA-256) so the stored {salt, verifier} authenticates (SAD §2.1, §5.1).
    srp::Parameters srp_params{};  // {Rfc5054_2048, Sha256}

    // Session-grant lifetime: expires_at = now + this many seconds (SAD §2.1,
    // §4.1 "30 s expiry"). The reconnect window surfaced to the client (ISSUE
    // #66, auth.fbs SessionGrant.reconnect_window_ms).
    std::uint32_t grant_ttl_seconds = 30;
    std::uint32_t reconnect_window_ms = 30000;

    // OPS-05 metrics label (docs/telemetry-architecture.md §5): the `realm` label
    // value the auth-outcome + SRP-timing series carry (meridian_auth_*). authd is
    // realm-agnostic (it serves the realm LIST), so this is the operator's realm
    // name for grouping; defaults to "reference" (matching the ops stack labels).
    std::string realm_label = "reference";

    // OPS-05 session-flow trace exporter (#166; docs/telemetry-architecture.md
    // §5.3). When set, run_login emits an "authd.login" span (SRP verify → grant
    // issue) with outcome/realm attributes to this exporter; on a granted login the
    // span's ids are DERIVED from the grant_id (trace::flow::trace_context_from_
    // grant) so the worldd enter-world span stitches into the same trace (D-29 §9
    // rule 5). NULL => no tracing (graceful degradation — the login is unaffected).
    // Borrowed, not owned; must outlive the run_login call (main() owns it).
    meridian::trace::Exporter* tracer_exporter = nullptr;
};

// Outcome of one login attempt — for the caller's logging / metrics and for the
// integration test's assertions. Distinguishes "clean protocol rejection"
// (an Error was sent, connection ends normally) from a transport failure.
enum class LoginOutcome {
    kGranted,           // full flow completed; a SessionGrant was written + sent
    kRejectedHello,     // ClientHello failed the build/proto gate (Error sent)
    kRejectedAuth,      // SRP proof failed (AuthResult error sent); NO grant
    kRejectedRealm,     // RealmSelect named an unknown/unavailable realm (Error)
    kProtocolError,     // peer sent an unexpected/undecodable message
    kTransportClosed,   // peer closed the connection at a frame boundary
};

// Detail carried alongside the outcome (grant id when kGranted, else 0).
struct LoginResult {
    LoginOutcome outcome = LoginOutcome::kProtocolError;
    std::uint64_t grant_id = 0;   // set iff kGranted
    std::uint64_t account_id = 0; // resolved account (0 if never resolved)
    std::uint8_t gm_level = 0;    // resolved account's GM level (D-16; #417). authd
                                  // reads account.gm_level at SrpStart and exposes it
                                  // here so the login audit records WHO the actor is
                                  // AND their privilege tier. 0 (player) until resolved.
    std::string detail;           // human-readable note for logs

    // OPS-05 log↔trace correlation (#166 + #165). When a trace exporter is
    // configured, run_login stamps these with the lower-hex ids of the emitted
    // "authd.login" span so the caller can put them in the structured log's
    // `trace_id`/`span_id` fields — one grep pivots from a Loki log line to the
    // matching span in the trace backend (docs/telemetry-architecture.md §5.2/§5.3).
    // Empty when tracing is off.
    std::string trace_id;   // 32 lower-hex chars, or empty
    std::string span_id;    // 16 lower-hex chars, or empty
};

// Drive the full IF-1 login flow for one client over `sess`, using `db` for the
// account/realm/grant queries. Reads and writes framed FlatBuffers per the SAD
// §5.1 sequence; on any protocol/auth/realm failure it sends the defined Error /
// AuthResult and returns the corresponding outcome (it does NOT throw for those
// — they are normal protocol outcomes). It may throw meridian::net::TlsError on
// a transport failure and meridian::db::DbError on a DB failure; the caller's
// per-connection handler catches those and closes the connection.
//
// UNKNOWN-USER HANDLING (anti-enumeration, SAD §2.1 "passwords never leaked"):
// when SrpStart names an account that does not exist, authd does NOT reveal that
// fact. It derives a stable per-username fake salt (HMAC-style: a hash of the
// username under a server secret) and a throwaway verifier, runs a real SRP
// ServerSession against them, and returns AuthResult BAD_CREDENTIALS on proof —
// indistinguishable, from the client's side, from a wrong password for a real
// account. This costs one extra SRP handshake but removes the username oracle.
LoginResult run_login(net::Session& sess, db::Connection& db,
                      const LoginConfig& cfg);

}  // namespace meridian::authd
