// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — CLIENT connection state machine + reconnect policy (issue
// #96). The `net`-module FSM the client SAD §2.1 / §5.1 calls for:
//
//   Disconnected → Connecting → Authenticating → EnteringWorld → InWorld
//                                                     ↑             │
//                                                     └─ Reconnecting ← (dropped)
//                                                     │
//                                                     └→ Failed (window / attempts exhausted)
//
// This is a PURE state machine: it owns the session-lifecycle states, the events
// that drive them, and the reconnect BACKOFF policy — but performs NO I/O. It does
// not know about sockets, TLS, FlatBuffers, or worldd. That keeps it engine-free
// (client SAD §9.2) and testable as a deterministic FSM: feed it events + a virtual
// clock, assert the state and the next backoff delay. The I/O half (drive a real
// re-establish over ILoginTransport) is the ReconnectCoordinator (reconnect_
// coordinator.*), which owns a ConnectionFsm and turns transport outcomes into
// these events.
//
// ── THE M0 RECONNECT REALITY (honest, read from the server on `main`) ──────────
// The IT-M0 runbook step 4 asks for "reconnect works: a transient drop within the
// reconnect window resumes the session without a relog." The SAD §5.1 reconnect
// POLICY (enter Reconnecting on transient IF-2 loss, exponential backoff, ~30 s
// window, then full re-login) is what THIS FSM implements.
//
// But a TRUE token-reconnect (re-present the SAME grant to worldd to RESUME the
// dropped session) is NOT supported server-side at M0:
//   • worldd's session grant is SINGLE-USE — validate_and_consume_grant() atomically
//     sets consumed_at on first enter-world (server/worldd/world_session.cpp #84);
//     a re-presented grant affects 0 rows → GRANT_INVALID.
//   • world.fbs has NO reconnect / resume opcode (only WORLD_HELLO/HANDSHAKE_OK/
//     DISCONNECT/CLOCK_SYNC in the session range).
//   • The runbook itself codifies it: "reconnect = single-use grant re-auth"
//     (docs/it-m0-runbook.md §Traceability; auth.fbs header: "reconnect = full
//     re-auth"). The grant's expires_at TTL (~30 s) EQUALS reconnect_window_ms —
//     the window is how long a grant stays enter-able, not a resume budget for an
//     already-consumed session.
//
// So this FSM models BOTH strategies via ReconnectStrategy so the client is correct
// whichever way the server goes:
//   • kResumeWithGrant  — re-present the same grant (a true token-reconnect). This
//     is the seam that lights up the instant worldd grows a server-side resume
//     path; TODAY it is rejected GRANT_INVALID, which the coordinator surfaces
//     honestly (and the FSM treats as a failed attempt → backoff → give up).
//   • kFullRelogin      — the WORKING M0 path: a fresh authd login yields a fresh
//     grant, re-enter world. "Reconnect" without a relog is a SERVER FOLLOW-UP.
//
// The FSM is strategy-agnostic: it counts attempts, schedules backoff, and honours
// the window budget identically. The strategy only changes what the coordinator
// DOES on a ReconnectAttempt — the state graph is the same.

#ifndef MERIDIAN_BOT_CONNECTION_FSM_H
#define MERIDIAN_BOT_CONNECTION_FSM_H

#include <cstdint>
#include <string>

namespace meridian::bot {

// ---------------------------------------------------------------------------
// States — the session lifecycle (client SAD §2.1 connection state machine).
// ---------------------------------------------------------------------------
enum class ConnState : std::uint8_t {
    kDisconnected = 0,   // no connection; the initial + terminal-clean state
    kConnecting,         // opening the transport to the server (TLS handshake)
    kAuthenticating,     // IF-1 login in flight (authd; SRP → SessionGrant)
    kEnteringWorld,      // IF-2 WorldHello sent, awaiting HandshakeOk (worldd)
    kInWorld,            // HandshakeOk received — the live in-world session
    kReconnecting,       // a transient drop; retrying (backoff) within the window
    kFailed,             // gave up (window expired / attempts exhausted / fatal)
    kOutOfDate,          // schema/protocol version mismatch — "client out of date"
                         // (issue #98). A DISTINCT terminal state from kFailed: it is
                         // user-actionable (update the client), NOT a network error, so
                         // the UX prompts an update rather than a retry/"try again".
};

const char* to_string(ConnState s);

// ---------------------------------------------------------------------------
// Events — the inputs that drive transitions. The coordinator turns transport
// outcomes into these; a unit test feeds them directly.
// ---------------------------------------------------------------------------
enum class ConnEvent : std::uint8_t {
    kConnect = 0,        // begin: open the transport (Disconnected → Connecting)
    kConnected,          // transport up (Connecting → Authenticating)
    kAuthOk,             // login succeeded, grant in hand (Authenticating → EnteringWorld)
    kEntered,            // HandshakeOk received (EnteringWorld → InWorld)
    kDropped,            // the live/handshaking connection was lost (→ Reconnecting)
    kReconnectAttempt,   // a backoff timer fired; a resume/relogin attempt begins
    kReconnectFailed,    // that attempt failed (→ schedule next backoff, or give up)
    kGaveUp,             // window expired / attempts exhausted (→ Failed) — explicit
    kFatal,              // an unrecoverable error (bad credentials, protocol) (→ Failed)
    kVersionMismatch,    // schema/protocol version check failed at connect (issue #98):
                         // the client is out of date (→ OutOfDate, a DISTINCT terminal
                         // from Failed — an update is required, not a reconnect)
    kReset,              // return to Disconnected (clean teardown / new session)
};

const char* to_string(ConnEvent e);

// ---------------------------------------------------------------------------
// Which reconnect strategy the coordinator will use on each ReconnectAttempt.
// The FSM records it for reporting; it does NOT change the state graph.
// ---------------------------------------------------------------------------
enum class ReconnectStrategy : std::uint8_t {
    // Re-present the SAME session grant to worldd to RESUME the dropped session.
    // A true token-reconnect. NOT supported server-side at M0 (single-use grant) —
    // the coordinator will see GRANT_INVALID. Kept as the forward-looking seam.
    kResumeWithGrant = 0,
    // Re-run the full authd login to get a FRESH grant, then re-enter world. The
    // path that actually works at M0 — but it is a re-login, not a resume.
    kFullRelogin = 1,
};

const char* to_string(ReconnectStrategy s);

// ---------------------------------------------------------------------------
// Backoff policy — exponential backoff with a cap, a max-attempts ceiling, AND a
// hard window budget (the server-owned reconnect_window_ms from the grant). A
// reconnect gives up on the FIRST of: attempts exhausted OR window elapsed.
// ---------------------------------------------------------------------------
struct BackoffPolicy {
    std::uint32_t base_delay_ms = 250;    // first retry delay
    std::uint32_t max_delay_ms = 4000;    // per-attempt cap (delay never exceeds this)
    double factor = 2.0;                  // exponential multiplier per attempt
    std::uint32_t max_attempts = 6;       // ceiling on retries before giving up
    // The server-owned reconnect window (SessionGrant.reconnect_window_ms, #66).
    // Elapsed reconnecting-time past this → give up (grant presumed expired). 0
    // disables the window check (attempts ceiling only).
    std::uint32_t window_ms = 30000;

    // The delay before attempt `n` (n = 0 is the first retry). Exponential, capped
    // at max_delay_ms. Pure function of the policy — deterministic, no jitter (the
    // FSM stays testable; jitter, if wanted, is added by the coordinator's timer).
    std::uint32_t delay_for_attempt(std::uint32_t n) const;
};

// ---------------------------------------------------------------------------
// ConnectionFsm — the pure state machine. Not thread-safe (one session, one
// driver thread, mirroring the synchronous bot). Time is INJECTED (elapsed ms
// since reconnect began) so tests are deterministic; no wall clock inside.
// ---------------------------------------------------------------------------
class ConnectionFsm {
public:
    ConnectionFsm() = default;
    explicit ConnectionFsm(BackoffPolicy policy,
                           ReconnectStrategy strategy = ReconnectStrategy::kFullRelogin)
        : policy_(policy), strategy_(strategy) {}

    ConnState state() const { return state_; }
    ReconnectStrategy strategy() const { return strategy_; }
    const BackoffPolicy& policy() const { return policy_; }

    // How many reconnect ATTEMPTS have been started in the current drop episode.
    std::uint32_t attempts() const { return attempts_; }
    // The delay (ms) scheduled before the NEXT reconnect attempt (valid while in
    // Reconnecting after a drop / failed attempt). 0 if not scheduling one.
    std::uint32_t next_backoff_ms() const { return next_backoff_ms_; }
    // True once the machine reached the live in-world session at least once. Lets
    // the coordinator distinguish "dropped from InWorld" (resume) from "never got
    // in" (a login failure is not a reconnect case).
    bool ever_in_world() const { return ever_in_world_; }
    // Terminal = no further transition without a Reset. BOTH kFailed (gave up / fatal)
    // and kOutOfDate (client out of date, #98) are terminal — the caller tears the
    // session down — but they mean different things to the UX (retry-exhausted vs
    // update-required), so is_out_of_date() distinguishes them.
    bool is_terminal() const {
        return state_ == ConnState::kFailed || state_ == ConnState::kOutOfDate;
    }
    // True iff the session ended because the client's schema/protocol version was
    // rejected (#98). The UX shows "client out of date — please update", NOT a network
    // error — this is how a version mismatch is kept distinct from kFailed.
    bool is_out_of_date() const { return state_ == ConnState::kOutOfDate; }
    bool in_world() const { return state_ == ConnState::kInWorld; }

    // Apply `ev`. `elapsed_reconnect_ms` is the time spent reconnecting since the
    // drop that opened the current episode (used only for the window-budget check
    // on kReconnectAttempt / kReconnectFailed; ignored otherwise). Returns true if
    // the event caused a transition, false if it was ignored (illegal for the
    // current state — the FSM never throws or asserts on a stray event; it stays
    // put, so a noisy driver can't corrupt it).
    bool dispatch(ConnEvent ev, std::uint64_t elapsed_reconnect_ms = 0);

    // Convenience: would a reconnect give up right now, given `elapsed_reconnect_ms`?
    // True if attempts are exhausted OR the window budget is spent. Pure query.
    bool should_give_up(std::uint64_t elapsed_reconnect_ms) const;

private:
    // Enter Reconnecting from a drop: reset the episode counters and schedule the
    // first backoff. Called on kDropped from a state that had reached (or was
    // reaching) the world.
    void begin_reconnect_episode();
    // Schedule the backoff for the upcoming attempt (sets next_backoff_ms_ from the
    // policy for the current attempts_ count).
    void schedule_next_backoff();

    ConnState state_ = ConnState::kDisconnected;
    BackoffPolicy policy_{};
    ReconnectStrategy strategy_ = ReconnectStrategy::kFullRelogin;

    std::uint32_t attempts_ = 0;         // reconnect attempts in the current episode
    std::uint32_t next_backoff_ms_ = 0;  // delay before the next attempt
    bool ever_in_world_ = false;         // reached InWorld at least once
};

}  // namespace meridian::bot

#endif  // MERIDIAN_BOT_CONNECTION_FSM_H
