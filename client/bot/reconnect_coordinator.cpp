// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — reconnect coordinator implementation (#96). Drives the pure
// ConnectionFsm over an injected re-establish seam + clock. See the header for the
// two strategies and the honest "resumed vs relogin" verdict.

#include "reconnect_coordinator.h"

namespace meridian::bot {

const char* to_string(ReEstablishOutcome o) {
    switch (o) {
        case ReEstablishOutcome::kEnteredWorld:  return "EnteredWorld";
        case ReEstablishOutcome::kGrantRejected: return "GrantRejected";
        case ReEstablishOutcome::kConnectFailed: return "ConnectFailed";
        case ReEstablishOutcome::kFatal:         return "Fatal";
    }
    return "?";
}

ReconnectReport run_reconnect(ConnectionFsm& fsm, const ReEstablishFn& reestablish,
                              const NowFn& now, const WaitFn& wait, bool drop_first) {
    ReconnectReport rep;

    const std::uint64_t episode_start = now();

    // If asked, issue the drop that opens the episode (InWorld → Reconnecting).
    if (drop_first) {
        fsm.dispatch(ConnEvent::kDropped);
    }

    // The backoff/attempt loop. It runs while the FSM is Reconnecting. Each pass
    // waits the scheduled backoff, tells the FSM an attempt is starting (which does
    // the budget check and may transition to Failed), then — if still trying —
    // performs the re-establish and maps its outcome back to the FSM.
    while (fsm.state() == ConnState::kReconnecting) {
        const std::uint32_t backoff = fsm.next_backoff_ms();
        if (backoff > 0) wait(backoff);

        const std::uint64_t elapsed = now() - episode_start;

        // Start the attempt (budget check inside). If the budget is spent the FSM
        // goes Failed here and the loop exits without a doomed network call.
        fsm.dispatch(ConnEvent::kReconnectAttempt, elapsed);
        if (fsm.state() != ConnState::kReconnecting) break;  // gave up on budget

        const std::uint32_t attempt_index = fsm.attempts();  // 1-based after dispatch
        const ReEstablishOutcome outcome = reestablish(attempt_index, fsm.strategy());

        AttemptRecord rec;
        rec.attempt_index = attempt_index;
        rec.backoff_ms = backoff;
        rec.strategy = fsm.strategy();
        rec.outcome = outcome;
        rec.elapsed_ms = elapsed;
        rep.attempt_log.push_back(rec);

        switch (outcome) {
            case ReEstablishOutcome::kEnteredWorld:
                fsm.dispatch(ConnEvent::kEntered, elapsed);
                // HONEST verdict: a resume WITHOUT a relogin only if the strategy
                // was to re-present the same grant. A full relogin re-entered the
                // world, but it is a NEW session, not a resume.
                if (rec.strategy == ReconnectStrategy::kResumeWithGrant) {
                    rep.resumed_without_relogin = true;
                }
                break;
            case ReEstablishOutcome::kGrantRejected:
            case ReEstablishOutcome::kConnectFailed:
                // Retryable: schedule the next backoff (or give up if budget spent).
                fsm.dispatch(ConnEvent::kReconnectFailed, now() - episode_start);
                break;
            case ReEstablishOutcome::kFatal:
                fsm.dispatch(ConnEvent::kFatal, elapsed);
                break;
        }
    }

    rep.final_state = fsm.state();
    rep.reconnected = (fsm.state() == ConnState::kInWorld);
    rep.gave_up = (fsm.state() == ConnState::kFailed);
    rep.attempts = fsm.attempts();
    rep.total_reconnect_ms = now() - episode_start;

    if (rep.reconnected) {
        rep.detail = rep.resumed_without_relogin
                         ? "reconnected: session RESUMED with token (no relogin)"
                         : "reconnected: via full re-login (fresh grant — NOT a token resume)";
    } else if (rep.gave_up) {
        rep.detail = "gave up: reconnect window / attempts exhausted";
    } else {
        rep.detail = "reconnect ended in a non-terminal state";
    }
    return rep;
}

}  // namespace meridian::bot
