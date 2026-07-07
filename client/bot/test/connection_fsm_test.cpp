// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free unit tests for the CLIENT connection state machine
// + reconnect coordinator (#96). Plain-main style (no Godot, no doctest), mirroring
// the client/server core tests. ctest-wired via client/bot/CMakeLists.txt.
//
// This test needs NO OpenSSL / FlatBuffers / sockets — the FSM is a PURE state
// machine and the coordinator drives it over an injected re-establish seam + a
// virtual clock. That is the point: the reconnect POLICY is provable deterministically
// with no server and no wall-clock sleeps. (The live/mock enter-world interop is the
// bot-world-session test + the integration harness.)
//
// Proves the #96 deliverables:
//   1. HAPPY PATH transitions: Disconnected → Connecting → Authenticating →
//      EnteringWorld → InWorld.
//   2. RECONNECT SUCCESS: InWorld → (Dropped) → Reconnecting → (Entered) → InWorld.
//   3. RECONNECT GIVE-UP (attempts): repeated ReconnectFailed exhausts max_attempts
//      → Failed.
//   4. RECONNECT GIVE-UP (window): elapsed reconnect time ≥ window_ms → Failed even
//      with attempts left.
//   5. BACKOFF policy: exponential with a cap; the FSM schedules the right delay per
//      attempt.
//   6. ILLEGAL events are ignored (no transition, no corruption).
//   7. COORDINATOR happy resume: kResumeWithGrant + a mock that "enters" →
//      resumed_without_relogin == true.
//   8. COORDINATOR M0 reality: kResumeWithGrant against a mock that rejects the
//      re-presented grant (worldd single-use) → falls back through backoff, gives
//      up, resumed_without_relogin == false (the HONEST server-gap proof).
//   9. COORDINATOR relogin path: kFullRelogin + a mock that enters on attempt N →
//      reconnected == true but resumed_without_relogin == false (a relogin, not a
//      resume). Backoff waits are counted via the virtual clock.

#include "connection_fsm.h"
#include "reconnect_coordinator.h"

#include <cstdio>
#include <vector>

using namespace meridian::bot;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

// Drive the FSM through the full happy handshake to InWorld. Returns the fsm.
static void drive_to_in_world(ConnectionFsm& fsm) {
    fsm.dispatch(ConnEvent::kConnect);
    fsm.dispatch(ConnEvent::kConnected);
    fsm.dispatch(ConnEvent::kAuthOk);
    fsm.dispatch(ConnEvent::kEntered);
}

int main() {
    std::printf("connection FSM + reconnect coordinator tests (#96)\n\n");

    // ===== 1. Happy-path transitions =======================================
    std::printf("1. happy-path lifecycle transitions\n");
    {
        ConnectionFsm fsm;
        check("1: starts Disconnected", fsm.state() == ConnState::kDisconnected);
        check("1: Connect -> Connecting",
              fsm.dispatch(ConnEvent::kConnect) && fsm.state() == ConnState::kConnecting);
        check("1: Connected -> Authenticating",
              fsm.dispatch(ConnEvent::kConnected) && fsm.state() == ConnState::kAuthenticating);
        check("1: AuthOk -> EnteringWorld",
              fsm.dispatch(ConnEvent::kAuthOk) && fsm.state() == ConnState::kEnteringWorld);
        check("1: Entered -> InWorld",
              fsm.dispatch(ConnEvent::kEntered) && fsm.state() == ConnState::kInWorld);
        check("1: in_world() true", fsm.in_world());
        check("1: ever_in_world() true", fsm.ever_in_world());
    }

    // ===== 2. Reconnect SUCCESS: drop -> Reconnecting -> InWorld =============
    std::printf("\n2. reconnect success (drop -> Reconnecting -> InWorld)\n");
    {
        ConnectionFsm fsm;
        drive_to_in_world(fsm);
        check("2: InWorld before drop", fsm.state() == ConnState::kInWorld);

        check("2: Dropped -> Reconnecting",
              fsm.dispatch(ConnEvent::kDropped) && fsm.state() == ConnState::kReconnecting);
        check("2: first backoff scheduled (>0)", fsm.next_backoff_ms() > 0);
        check("2: attempts reset to 0", fsm.attempts() == 0);

        // A backoff timer fires -> an attempt begins.
        check("2: ReconnectAttempt stays Reconnecting",
              fsm.dispatch(ConnEvent::kReconnectAttempt, /*elapsed=*/300) &&
                  fsm.state() == ConnState::kReconnecting);
        check("2: attempts == 1 after first attempt", fsm.attempts() == 1);

        // The attempt succeeds (re-entered the world).
        check("2: Entered -> InWorld (reconnected)",
              fsm.dispatch(ConnEvent::kEntered, /*elapsed=*/350) &&
                  fsm.state() == ConnState::kInWorld);
        check("2: not terminal", !fsm.is_terminal());
    }

    // ===== 3. Reconnect GIVE-UP by attempts ceiling =========================
    std::printf("\n3. reconnect give-up (attempts exhausted -> Failed)\n");
    {
        BackoffPolicy p;
        p.max_attempts = 3;
        p.window_ms = 0;  // disable window; test the attempts ceiling alone
        ConnectionFsm fsm(p);
        drive_to_in_world(fsm);
        fsm.dispatch(ConnEvent::kDropped);

        // Three failed attempts should exhaust the ceiling.
        for (int i = 1; i <= 3; ++i) {
            check("3: attempt N stays Reconnecting (elapsed small)",
                  fsm.dispatch(ConnEvent::kReconnectAttempt, /*elapsed=*/10) &&
                      (fsm.state() == ConnState::kReconnecting));
            fsm.dispatch(ConnEvent::kReconnectFailed, /*elapsed=*/10);
        }
        // The 3rd failure (attempts_==3, max==3) should have gone Failed.
        check("3: Failed after max_attempts failures", fsm.state() == ConnState::kFailed);
        check("3: is_terminal()", fsm.is_terminal());
        // A fresh attempt on a Failed FSM is ignored.
        check("3: ReconnectAttempt ignored when Failed",
              !fsm.dispatch(ConnEvent::kReconnectAttempt, 10) &&
                  fsm.state() == ConnState::kFailed);
    }

    // ===== 4. Reconnect GIVE-UP by window budget ============================
    std::printf("\n4. reconnect give-up (window budget spent -> Failed)\n");
    {
        BackoffPolicy p;
        p.max_attempts = 100;   // effectively unlimited attempts
        p.window_ms = 30000;    // 30 s window (the #66 grant reconnect_window_ms)
        ConnectionFsm fsm(p);
        drive_to_in_world(fsm);
        fsm.dispatch(ConnEvent::kDropped);

        // An attempt well within the window proceeds.
        check("4: attempt within window proceeds",
              fsm.dispatch(ConnEvent::kReconnectAttempt, /*elapsed=*/5000) &&
                  fsm.state() == ConnState::kReconnecting);
        fsm.dispatch(ConnEvent::kReconnectFailed, /*elapsed=*/5000);
        check("4: still Reconnecting mid-window", fsm.state() == ConnState::kReconnecting);

        // An attempt AT/BEYOND the window gives up even though attempts remain.
        check("4: attempt at window boundary -> Failed",
              fsm.dispatch(ConnEvent::kReconnectAttempt, /*elapsed=*/30000) &&
                  fsm.state() == ConnState::kFailed);
        check("4: should_give_up true past window",
              ConnectionFsm(p).should_give_up(30001) == true);
    }

    // ===== 5. Backoff policy (exponential + cap) ============================
    std::printf("\n5. backoff policy (exponential with cap)\n");
    {
        BackoffPolicy p;
        p.base_delay_ms = 250;
        p.factor = 2.0;
        p.max_delay_ms = 4000;
        check("5: attempt 0 = base (250)", p.delay_for_attempt(0) == 250);
        check("5: attempt 1 = 500", p.delay_for_attempt(1) == 500);
        check("5: attempt 2 = 1000", p.delay_for_attempt(2) == 1000);
        check("5: attempt 3 = 2000", p.delay_for_attempt(3) == 2000);
        check("5: attempt 4 = 4000", p.delay_for_attempt(4) == 4000);
        check("5: attempt 5 capped at 4000", p.delay_for_attempt(5) == 4000);
        check("5: large attempt still capped", p.delay_for_attempt(40) == 4000);

        // The FSM schedules base for the first attempt after a drop.
        ConnectionFsm fsm(p);
        drive_to_in_world(fsm);
        fsm.dispatch(ConnEvent::kDropped);
        check("5: FSM first backoff == base", fsm.next_backoff_ms() == 250);
        fsm.dispatch(ConnEvent::kReconnectAttempt, 10);
        fsm.dispatch(ConnEvent::kReconnectFailed, 10);
        check("5: FSM second backoff == 500 (attempt 1)", fsm.next_backoff_ms() == 500);
    }

    // ===== 6. Illegal events are ignored (no corruption) ====================
    std::printf("\n6. illegal events ignored\n");
    {
        ConnectionFsm fsm;
        check("6: Entered ignored while Disconnected",
              !fsm.dispatch(ConnEvent::kEntered) && fsm.state() == ConnState::kDisconnected);
        fsm.dispatch(ConnEvent::kConnect);
        check("6: AuthOk ignored while Connecting",
              !fsm.dispatch(ConnEvent::kAuthOk) && fsm.state() == ConnState::kConnecting);
        // Reset from anywhere returns to Disconnected.
        drive_to_in_world(fsm);  // (Connect was already applied; drive the rest)
        // fsm may be InWorld now; Reset must clean it.
        check("6: Reset -> Disconnected",
              fsm.dispatch(ConnEvent::kReset) && fsm.state() == ConnState::kDisconnected);
        check("6: Reset clears ever_in_world", !fsm.ever_in_world());
    }

    // ===== 6b. Fatal from a live state is terminal ==========================
    std::printf("\n6b. fatal error -> Failed\n");
    {
        ConnectionFsm fsm;
        fsm.dispatch(ConnEvent::kConnect);
        fsm.dispatch(ConnEvent::kConnected);
        check("6b: Fatal in Authenticating -> Failed",
              fsm.dispatch(ConnEvent::kFatal) && fsm.state() == ConnState::kFailed);
        check("6b: Fatal ignored when already Disconnected",
              !ConnectionFsm().dispatch(ConnEvent::kFatal));
    }

    // ===== 6c. A pre-InWorld drop is NOT a reconnect episode =================
    std::printf("\n6c. drop before InWorld falls back to Disconnected\n");
    {
        ConnectionFsm fsm;
        fsm.dispatch(ConnEvent::kConnect);
        fsm.dispatch(ConnEvent::kConnected);  // Authenticating
        check("6c: drop while Authenticating -> Disconnected (not Reconnecting)",
              fsm.dispatch(ConnEvent::kDropped) && fsm.state() == ConnState::kDisconnected);
        check("6c: never reached world", !fsm.ever_in_world());
    }

    // ===================================================================== //
    //  Reconnect COORDINATOR — the FSM driven over an injected seam + clock  //
    // ===================================================================== //

    // A virtual clock the WaitFn advances (no real sleeps). NowFn returns it.
    struct VClock {
        std::uint64_t t = 0;
    };

    // ===== 7. Coordinator: TRUE token-resume succeeds (forward-looking) =====
    // Model a HYPOTHETICAL worldd that DOES support resume: kResumeWithGrant enters
    // the world on the first attempt. resumed_without_relogin must be true. This
    // proves the client is READY the instant the server grows a resume path.
    std::printf("\n7. coordinator: token-resume succeeds (if server supported it)\n");
    {
        VClock clk;
        BackoffPolicy p;
        p.base_delay_ms = 200;
        ConnectionFsm fsm(p, ReconnectStrategy::kResumeWithGrant);
        drive_to_in_world(fsm);

        auto now = [&clk]() { return clk.t; };
        auto wait = [&clk](std::uint32_t ms) { clk.t += ms; };
        auto reestablish = [](std::uint32_t attempt, ReconnectStrategy s) {
            (void)attempt;
            (void)s;
            return ReEstablishOutcome::kEnteredWorld;  // server resumed us
        };

        ReconnectReport rep = run_reconnect(fsm, reestablish, now, wait, /*drop_first=*/true);
        check("7: reconnected", rep.reconnected);
        check("7: RESUMED without relogin (token-reconnect)", rep.resumed_without_relogin);
        check("7: final state InWorld", rep.final_state == ConnState::kInWorld);
        check("7: exactly 1 attempt", rep.attempts == 1);
        check("7: waited the base backoff once", rep.total_reconnect_ms == 200);
        check("7: attempt log has 1 entry", rep.attempt_log.size() == 1);
    }

    // ===== 8. Coordinator: M0 REALITY — worldd rejects the re-presented grant
    // (single-use). kResumeWithGrant can NEVER resume; it exhausts backoff and gives
    // up. resumed_without_relogin is false. THIS is the honest server-gap proof.
    std::printf("\n8. coordinator: M0 reality — single-use grant, resume rejected\n");
    {
        VClock clk;
        BackoffPolicy p;
        p.base_delay_ms = 100;
        p.factor = 2.0;
        p.max_delay_ms = 800;
        p.max_attempts = 4;
        p.window_ms = 0;  // isolate the attempts ceiling (window tested in #4)
        ConnectionFsm fsm(p, ReconnectStrategy::kResumeWithGrant);
        drive_to_in_world(fsm);

        auto now = [&clk]() { return clk.t; };
        auto wait = [&clk](std::uint32_t ms) { clk.t += ms; };
        int calls = 0;
        auto reestablish = [&calls](std::uint32_t attempt, ReconnectStrategy s) {
            ++calls;
            (void)attempt;
            (void)s;
            // worldd validate_and_consume_grant: the grant was consumed on the first
            // enter-world, so a re-presentation affects 0 rows -> GRANT_INVALID.
            return ReEstablishOutcome::kGrantRejected;
        };

        ReconnectReport rep = run_reconnect(fsm, reestablish, now, wait, /*drop_first=*/true);
        check("8: did NOT reconnect", !rep.reconnected);
        check("8: NOT resumed without relogin (honest gap)", !rep.resumed_without_relogin);
        check("8: gave up", rep.gave_up);
        check("8: final state Failed", rep.final_state == ConnState::kFailed);
        check("8: made max_attempts attempts", rep.attempts == 4);
        check("8: reestablish called once per attempt", calls == 4);
        // Backoff progression waited: 100 + 200 + 400 + 800 = 1500 ms.
        check("8: total backoff waited = 100+200+400+800", rep.total_reconnect_ms == 1500);
    }

    // ===== 9. Coordinator: kFullRelogin — the WORKING M0 path (a relogin) ===
    // A fresh login yields a fresh grant; re-enter world on attempt 2 (attempt 1
    // still connect-failed, exercising a retry). reconnected == true, but
    // resumed_without_relogin == false: it is a RE-LOGIN, not a session resume.
    std::printf("\n9. coordinator: full re-login reconnect (works at M0, but not a resume)\n");
    {
        VClock clk;
        BackoffPolicy p;
        p.base_delay_ms = 250;
        p.factor = 2.0;
        p.max_attempts = 5;
        p.window_ms = 0;
        ConnectionFsm fsm(p, ReconnectStrategy::kFullRelogin);
        drive_to_in_world(fsm);

        auto now = [&clk]() { return clk.t; };
        auto wait = [&clk](std::uint32_t ms) { clk.t += ms; };
        auto reestablish = [](std::uint32_t attempt, ReconnectStrategy s) {
            (void)s;
            if (attempt == 1) return ReEstablishOutcome::kConnectFailed;  // transient
            return ReEstablishOutcome::kEnteredWorld;  // fresh login succeeded
        };

        ReconnectReport rep = run_reconnect(fsm, reestablish, now, wait, /*drop_first=*/true);
        check("9: reconnected (re-entered world)", rep.reconnected);
        check("9: NOT a token resume (it was a relogin)", !rep.resumed_without_relogin);
        check("9: final state InWorld", rep.final_state == ConnState::kInWorld);
        check("9: took 2 attempts", rep.attempts == 2);
        // Waited base (250) before attempt 1, then attempt-1 backoff (500) before 2.
        check("9: total backoff = 250 + 500", rep.total_reconnect_ms == 750);
        check("9: attempt log records the strategy",
              rep.attempt_log.size() == 2 &&
                  rep.attempt_log[0].strategy == ReconnectStrategy::kFullRelogin);
    }

    // ===== 10. Coordinator: fatal on re-establish stops immediately =========
    std::printf("\n10. coordinator: fatal re-establish -> Failed, no further attempts\n");
    {
        VClock clk;
        BackoffPolicy p;
        p.base_delay_ms = 100;
        ConnectionFsm fsm(p, ReconnectStrategy::kFullRelogin);
        drive_to_in_world(fsm);
        auto now = [&clk]() { return clk.t; };
        auto wait = [&clk](std::uint32_t ms) { clk.t += ms; };
        int calls = 0;
        auto reestablish = [&calls](std::uint32_t, ReconnectStrategy) {
            ++calls;
            return ReEstablishOutcome::kFatal;  // e.g. credentials rejected on relogin
        };
        ReconnectReport rep = run_reconnect(fsm, reestablish, now, wait, /*drop_first=*/true);
        check("10: gave up (Failed)", rep.gave_up && rep.final_state == ConnState::kFailed);
        check("10: exactly one attempt before fatal", calls == 1);
        check("10: not reconnected", !rep.reconnected);
    }

    std::printf(g_fail == 0 ? "\nALL CONNECTION-FSM TESTS PASSED\n"
                            : "\n%d CONNECTION-FSM TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
