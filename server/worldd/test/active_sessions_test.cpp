// SPDX-License-Identifier: Apache-2.0
//
// worldd — single active session per account UNIT TEST (issue #326).
//
// CLEAN-ROOM: written from issue #326 + active_sessions.h only. No GPL source
// consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: drives ActiveSessionRegistry directly, so it runs
// in the PLAIN server ctest (no MariaDB service). The DB-backed WORLD_HELLO wiring
// (admit-on-enter, KickFn sends Disconnect{KICKED}) is proven end-to-end by the
// env-guarded worldd-session job; this pins the registry's core guarantee — the
// "1 login" half of the M0.5 concurrency task — as an always-green unit test.
//
// What it proves (the #326 acceptance: two concurrent logins for one account
// cannot both be in-world; the 2nd kicks the 1st):
//   A. ADMIT: a fresh account admits with no kick; the account is then active.
//   B. KICK-OLD: a 2nd login for the SAME account kicks the 1st (its KickFn fires
//      exactly once) and the account still has exactly ONE live session.
//   C. TOKEN GUARD: the kicked-old session's later release() is a NO-OP — it does
//      NOT evict the session that replaced it (compare-and-remove by token).
//   D. INDEPENDENCE: different accounts do not interfere (no cross-account kicks).
//   E. CLEAN RELEASE: the current holder's release() frees the slot; re-admitting
//      the same account afterwards does NOT kick (the slot was empty).
//   F. CONCURRENCY: many threads hammering admit/release for one account leave the
//      registry consistent (never more than one holder) and never crash.
//   G. DETERMINISTIC KICK UNDER CONCURRENCY: N threads that each admit and then
//      hold (barrier) before releasing overlap every admission, so EXACTLY N-1
//      kicks fire on every run — proving the kick path under real concurrency
//      without depending on incidental scheduling (issue #351: the old F asserted
//      "at least one kick happened", which CPU starvation could suppress by
//      serializing the admit/release loop so no admissions ever overlapped).

#include "active_sessions.h"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("meridian worldd single-active-session registry UNIT test (#326)\n");

    constexpr std::uint64_t kAcctA = 4242;
    constexpr std::uint64_t kAcctB = 9001;

    // ===== A. ADMIT a fresh account =========================================
    {
        ActiveSessionRegistry reg;
        int kicks = 0;
        AdmitResult r = reg.admit(kAcctA, [&kicks]() { ++kicks; });
        check("A: fresh admit does not kick", !r.kicked_previous && kicks == 0);
        check("A: admit returns a non-zero token", r.token != 0);
        check("A: account is now active", reg.is_active(kAcctA));
        check("A: exactly one active account", reg.active_count() == 1);
    }

    // ===== B. KICK-OLD on a 2nd login for the same account ==================
    {
        ActiveSessionRegistry reg;
        int first_kicked = 0;
        int second_kicked = 0;
        AdmitResult first = reg.admit(kAcctA, [&first_kicked]() { ++first_kicked; });
        AdmitResult second = reg.admit(kAcctA, [&second_kicked]() { ++second_kicked; });

        check("B: 2nd login reports it kicked the previous session",
              second.kicked_previous);
        check("B: the FIRST session's KickFn fired exactly once", first_kicked == 1);
        check("B: the SECOND session's KickFn has NOT fired", second_kicked == 0);
        check("B: still exactly one live session for the account",
              reg.active_count() == 1 && reg.is_active(kAcctA));
        check("B: the two admissions have distinct tokens",
              first.token != second.token);
    }

    // ===== C. TOKEN GUARD: kicked-old release() must not evict the new holder =
    {
        ActiveSessionRegistry reg;
        AdmitResult first = reg.admit(kAcctA, []() {});
        AdmitResult second = reg.admit(kAcctA, []() {});  // kicks `first`

        // The kicked-old session tears down and releases with ITS token.
        bool removed_by_old = reg.release(kAcctA, first.token);
        check("C: kicked-old release() removes nothing (not the holder)",
              !removed_by_old);
        check("C: the account is STILL active (new holder intact)",
              reg.is_active(kAcctA) && reg.active_count() == 1);

        // The current holder releases with its own token -> slot frees.
        bool removed_by_new = reg.release(kAcctA, second.token);
        check("C: current-holder release() frees the slot", removed_by_new);
        check("C: account no longer active", !reg.is_active(kAcctA) &&
                                                 reg.active_count() == 0);
    }

    // ===== D. INDEPENDENCE across accounts ==================================
    {
        ActiveSessionRegistry reg;
        int a_kicks = 0;
        int b_kicks = 0;
        reg.admit(kAcctA, [&a_kicks]() { ++a_kicks; });
        AdmitResult b = reg.admit(kAcctB, [&b_kicks]() { ++b_kicks; });
        check("D: admitting a different account kicks nobody",
              !b.kicked_previous && a_kicks == 0 && b_kicks == 0);
        check("D: both accounts are active", reg.is_active(kAcctA) &&
                                                 reg.is_active(kAcctB) &&
                                                 reg.active_count() == 2);
    }

    // ===== E. CLEAN RELEASE then re-admit does not kick =====================
    {
        ActiveSessionRegistry reg;
        AdmitResult first = reg.admit(kAcctA, []() {});
        check("E: release by holder succeeds", reg.release(kAcctA, first.token));
        int kicks = 0;
        AdmitResult again = reg.admit(kAcctA, [&kicks]() { ++kicks; });
        check("E: re-admit after clean release does not kick",
              !again.kicked_previous && kicks == 0);
        check("E: account active again with a fresh token",
              reg.is_active(kAcctA) && again.token != first.token);
    }

    // ===== F. CONCURRENCY: hammer one account from many threads =============
    {
        ActiveSessionRegistry reg;
        std::atomic<int> kicks{0};
        constexpr int kThreads = 8;
        constexpr int kIters = 500;
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&reg, &kicks]() {
                for (int i = 0; i < kIters; ++i) {
                    AdmitResult r = reg.admit(kAcctA, [&kicks]() {
                        kicks.fetch_add(1, std::memory_order_relaxed);
                    });
                    // Best-effort release; some will be no-ops (already kicked).
                    reg.release(kAcctA, r.token);
                }
            });
        }
        for (auto& th : ts) th.join();
        // Invariant: the registry never exposes more than one holder, and after
        // the storm it holds at most one account. No assertion on exact kick count
        // here (interleavings vary, and CPU starvation can serialize the loop so
        // no admissions overlap and zero kicks fire — the flake in issue #351) —
        // only that it stayed consistent and did not crash. The kick path under
        // concurrency is proven DETERMINISTICALLY in section G below.
        check("F: registry holds at most one account after the storm",
              reg.active_count() <= 1);
    }

    // ===== G. DETERMINISTIC KICK UNDER CONCURRENCY ==========================
    // Prove concurrent admits for one account kick the displaced holders WITHOUT
    // relying on incidental interleaving. A barrier holds every thread AFTER its
    // admit() and BEFORE its release(), so all kThreads admissions are live at
    // once: exactly one admit finds the slot empty, the other kThreads-1 each kick
    // the current holder. The kick count is therefore EXACTLY kThreads-1 on every
    // run, whatever the scheduler does — the deterministic replacement for the old
    // scheduling-dependent "at least one kick" assertion (issue #351).
    {
        ActiveSessionRegistry reg;
        std::atomic<int> kicks{0};
        constexpr int kThreads = 8;
        std::barrier all_admitted(kThreads);  // no release() runs until every admit() has
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&reg, &kicks, &all_admitted]() {
                AdmitResult r = reg.admit(kAcctA, [&kicks]() {
                    kicks.fetch_add(1, std::memory_order_relaxed);
                });
                all_admitted.arrive_and_wait();  // hold the slot until all threads admitted
                reg.release(kAcctA, r.token);
            });
        }
        for (auto& th : ts) th.join();
        check("G: N overlapping admits kick exactly N-1 displaced sessions",
              kicks.load() == kThreads - 1);
        check("G: every holder released -> registry empty",
              reg.active_count() == 0);
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD SINGLE-SESSION TESTS PASSED\n"
                            : "\n%d WORLDD SINGLE-SESSION TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
