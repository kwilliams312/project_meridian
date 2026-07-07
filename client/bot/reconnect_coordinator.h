// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — reconnect COORDINATOR (issue #96): the I/O half that drives
// the pure ConnectionFsm over a real (or mock) transport, turning a transient IF-2
// drop into a bounded, backed-off reconnect sequence and reporting — HONESTLY —
// whether the session RESUMED without a relog or needed a full re-login.
//
// This is the "reconnect-with-token" path the client SAD §5.1 / IT-M0 runbook step
// 4 call for. The coordinator owns:
//   • a ConnectionFsm (states + backoff policy);
//   • a re-establish SEAM (ReEstablishFn) the caller supplies — how to get a fresh
//     connection + (re)enter world. The seam abstracts login+worldd so the
//     coordinator is testable against a mock and reusable by the bot CLI / client.
//
// ── WHY A SEAM, AND THE TWO STRATEGIES ────────────────────────────────────────
// A ReconnectAttempt has to actually DO something. What it does depends on whether
// the server supports resuming a dropped session:
//
//   kResumeWithGrant — re-present the SAME grant to worldd (a true token-reconnect,
//     resuming without a relog). This is what "reconnect works" WANTS to mean. At
//     M0 worldd's grant is single-use (consumed on first enter-world), so this is
//     REJECTED GRANT_INVALID — the coordinator records that honestly and the FSM
//     treats it as a failed attempt. It is the forward-looking seam: the day worldd
//     grows a resume path, this strategy resumes for real with no client change to
//     the FSM.
//
//   kFullRelogin — re-run authd login for a FRESH grant, then enter world. This
//     WORKS at M0, but it is a re-login, not a resume: a new grant, a new session.
//     `resumed_without_relogin` is false for every attempt under this strategy.
//
// The coordinator is honest by construction: ReconnectReport.resumed_without_relogin
// is true ONLY if an attempt re-entered the world under kResumeWithGrant. Today, on
// the real server, it is always false — proving the server gap rather than papering
// over it. The bot integration test asserts the FSM sequence + the honest verdict.

#ifndef MERIDIAN_BOT_RECONNECT_COORDINATOR_H
#define MERIDIAN_BOT_RECONNECT_COORDINATOR_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "connection_fsm.h"

namespace meridian::bot {

// The outcome of ONE re-establish attempt (the seam's return). The coordinator maps
// this onto ConnEvents for the FSM.
enum class ReEstablishOutcome : std::uint8_t {
    kEnteredWorld = 0,   // reconnected + HandshakeOk — back in world (success)
    kGrantRejected,      // worldd said GRANT_INVALID (the M0 single-use reality on
                         // kResumeWithGrant; or an expired grant on kFullRelogin)
    kConnectFailed,      // could not reach the server (transport down) — retryable
    kFatal,              // an unrecoverable error (bad credentials on relogin, etc.)
};

const char* to_string(ReEstablishOutcome o);

// One reconnect attempt's record — for the honest report + the test assertions.
struct AttemptRecord {
    std::uint32_t attempt_index = 0;   // 1-based attempt number in the episode
    std::uint32_t backoff_ms = 0;      // the delay waited before this attempt
    ReconnectStrategy strategy = ReconnectStrategy::kFullRelogin;
    ReEstablishOutcome outcome = ReEstablishOutcome::kConnectFailed;
    std::uint64_t elapsed_ms = 0;      // reconnecting-time elapsed when it ran
};

// The honest end-of-reconnect report.
struct ReconnectReport {
    bool reconnected = false;              // did we get back InWorld at all?
    // TRUE only if reconnected via kResumeWithGrant (a genuine token-resume with NO
    // relogin). At M0 against real worldd this is ALWAYS false — the server has no
    // session-resume path (single-use grant). That is the honest headline.
    bool resumed_without_relogin = false;
    bool gave_up = false;                  // FSM reached Failed (attempts/window)
    ConnState final_state = ConnState::kDisconnected;
    std::uint32_t attempts = 0;            // reconnect attempts made
    std::uint64_t total_reconnect_ms = 0;  // reconnecting-time spent
    std::vector<AttemptRecord> attempt_log;
    std::string detail;
};

// The re-establish seam. Given the attempt index (1-based) and the strategy, do the
// work to re-enter the world and return the outcome. The coordinator supplies the
// attempt index + strategy; the implementation owns the transport + the grant. In
// the bot CLI this opens a fresh TLS connection to worldd (kResumeWithGrant: same
// grant) or re-runs authd login (kFullRelogin). In tests it is a lambda over a mock.
using ReEstablishFn =
    std::function<ReEstablishOutcome(std::uint32_t attempt_index, ReconnectStrategy)>;

// A monotonic clock seam (ms). Injected so tests use a virtual clock (no real
// sleeps) and production uses steady_clock. Also used to "wait" the backoff: the
// coordinator advances via WaitFn rather than sleeping, so the same code path is
// deterministic in tests and real-timed in the CLI.
using NowFn = std::function<std::uint64_t()>;
// Wait (block) for `ms` of backoff before the next attempt. Production sleeps; tests
// advance a virtual clock. Kept separate from NowFn so a test can advance time
// WITHOUT a wall-clock sleep and still have NowFn report the advanced time.
using WaitFn = std::function<void(std::uint32_t ms)>;

// Drive a reconnect episode after an in-world drop. Preconditions: `fsm` has just
// been given kDropped (state == Reconnecting) OR is InWorld and `drop_first` is
// true (the coordinator issues the kDropped). Runs the backoff/attempt loop:
//   loop:
//     wait(fsm.next_backoff_ms())
//     fsm.dispatch(ReconnectAttempt, elapsed)     // budget check; may → Failed
//     if Failed: break (gave up)
//     outcome = reestablish(attempt, strategy)
//     map outcome → Entered (success) | ReconnectFailed (retry) | Fatal (stop)
//     if InWorld: break (reconnected)
// Returns the honest ReconnectReport. Never sleeps in tests (WaitFn is injected).
ReconnectReport run_reconnect(ConnectionFsm& fsm, const ReEstablishFn& reestablish,
                              const NowFn& now, const WaitFn& wait,
                              bool drop_first = false);

}  // namespace meridian::bot

#endif  // MERIDIAN_BOT_RECONNECT_COORDINATOR_H
