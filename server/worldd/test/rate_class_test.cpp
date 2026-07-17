// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-opcode RATE CLASS unit test (OPS-03b, #421; epic #21).
//
// PURE / DB-FREE / SOCKET-FREE / SEEDED: drives the rate_class table + the
// per-connection RateLimiter directly with an INJECTED clock (the gate takes no
// clock itself), so every admit/drop decision is deterministic. Runs in the plain
// server ctest (no MERIDIAN_DB_* needed).
//
// CLEAN-ROOM: written from docs/prd/server-prd.md §4-M1 (OPS-03 per-opcode rate
// classes) + §6 (anti-cheat audit stream), rate_class.h, and the OPS-05 audit
// facility. No GPL source consulted (CONTRIBUTING).
//
// What it proves (the story's acceptance list):
//   A. The server-side table maps opcodes to their rate class + limits/names.
//   B. A CHAT FLOOD is throttled — the gate admits up to the chat ceiling in a
//      window, then DROPS the excess; a COMPLIANT chat rate always passes.
//   C. The RateLimiter's per-class buckets are INDEPENDENT — flooding one class
//      does not consume another's budget (≥ 2 classes exercised). (The dispatcher
//      DEFERS the move class to MovementIntake; the RateLimiter mechanism itself is
//      still per-class, which is what this asserts.)
//   D. The window SLIDES — a send one window later admits again.
//   E. A flood drop builds the anti-cheat audit record (kRateLimited, warn, the
//      offending opcode + the rate class as the reason, grant correlation), with
//      no secret material.

#include "rate_class.h"

#include "meridian/core/audit.hpp"

#include <cstdio>
#include <string>

using namespace meridian::worldd;
namespace mn = meridian::net;
namespace audit = meridian::core::audit;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// A — the server-side rate-class table + per-class limits/names.
void test_table() {
    std::printf("[rate] class table (OPS-03b)\n");
    check("A: MOVEMENT_INTENT -> move", rate_class_for(mn::Opcode::MOVEMENT_INTENT) == RateClass::kMove);
    check("A: CHAT_MESSAGE -> chat", rate_class_for(mn::Opcode::CHAT_MESSAGE) == RateClass::kChat);
    check("A: WORLD_HELLO -> session", rate_class_for(mn::Opcode::WORLD_HELLO) == RateClass::kSession);
    check("A: VENDOR_BUY_REQUEST -> action",
          rate_class_for(mn::Opcode::VENDOR_BUY_REQUEST) == RateClass::kAction);
    check("A: LOOT_TAKE -> action", rate_class_for(mn::Opcode::LOOT_TAKE) == RateClass::kAction);
    check("A: CAST_REQUEST -> action", rate_class_for(mn::Opcode::CAST_REQUEST) == RateClass::kAction);
    check("A: EQUIPMENT_CHANGE_REQUEST -> action",
          rate_class_for(mn::Opcode::EQUIPMENT_CHANGE_REQUEST) == RateClass::kAction);
    // Limits + names are the closed, documented set.
    check("A: chat limit", rate_class_limit(RateClass::kChat) == kChatRatePerWindow);
    check("A: action limit is the interactive flood ceiling",
          rate_class_limit(RateClass::kAction) == kActionRatePerWindow);
    check("A: every ceiling is a flood-level cap (>= 100/s)",
          rate_class_limit(RateClass::kSession) >= 100 &&
          rate_class_limit(RateClass::kChat) >= 100 &&
          rate_class_limit(RateClass::kAction) >= 100);
    check("A: class name chat", std::string(rate_class_name(RateClass::kChat)) == "chat");
    check("A: class name move", std::string(rate_class_name(RateClass::kMove)) == "move");
}

// B — a chat flood is dropped after the ceiling; a compliant rate always passes.
void test_chat_flood_vs_compliant() {
    std::printf("[rate] chat flood vs compliant rate\n");
    RateLimiter gate;
    const std::uint64_t t = 10'000;  // fixed instant — all within one window
    int admitted = 0;
    for (int i = 0; i < kChatRatePerWindow + 5; ++i) {
        if (gate.admit(RateClass::kChat, t)) ++admitted;
    }
    check("B: admits exactly the chat ceiling in one window", admitted == kChatRatePerWindow);
    check("B: the next chat send in the same window is dropped",
          !gate.admit(RateClass::kChat, t));
    check("B: dropped counter reflects the flood",
          gate.dropped(RateClass::kChat) == 6);  // the 5 loop overflow + the extra

    // A COMPLIANT session: one send well under the ceiling per window always passes.
    RateLimiter calm;
    bool all_ok = true;
    for (int w = 0; w < 10; ++w) {
        // 3 sends per second — under the chat ceiling — across ten seconds.
        const std::uint64_t base = static_cast<std::uint64_t>(w) * kRateWindowMs;
        for (int k = 0; k < 3; ++k) {
            if (!calm.admit(RateClass::kChat, base + static_cast<std::uint64_t>(k) * 100)) {
                all_ok = false;
            }
        }
    }
    check("B: a compliant chat rate never trips the gate", all_ok);
    check("B: a compliant session drops nothing", calm.dropped(RateClass::kChat) == 0);
}

// C — the RateLimiter's per-class buckets are independent (a 2nd class, action).
void test_class_buckets_independent() {
    std::printf("[rate] independent per-class buckets (2nd class)\n");
    RateLimiter gate;
    const std::uint64_t t = 20'000;
    int admitted = 0;
    for (int i = 0; i < kActionRatePerWindow + 3; ++i) {
        if (gate.admit(RateClass::kAction, t)) ++admitted;
    }
    check("C: admits exactly the action ceiling", admitted == kActionRatePerWindow);
    check("C: over the action ceiling is dropped", !gate.admit(RateClass::kAction, t));
    // The chat bucket on the SAME gate is untouched by the action flood.
    check("C: chat bucket independent of the action flood",
          gate.admit(RateClass::kChat, t) && gate.dropped(RateClass::kChat) == 0);
}

// D — the sliding window: a send one window later admits again.
void test_window_slides() {
    std::printf("[rate] window slide\n");
    RateLimiter gate;
    const std::uint64_t t = 30'000;
    for (int i = 0; i < kChatRatePerWindow; ++i) gate.admit(RateClass::kChat, t);
    check("D: at the ceiling, same-window send drops", !gate.admit(RateClass::kChat, t));
    check("D: a send one full window later admits again (window slid)",
          gate.admit(RateClass::kChat, t + kRateWindowMs));
}

// E — a flood drop builds the anti-cheat audit record.
void test_audit_record() {
    std::printf("[rate] anti-cheat audit record\n");
    const std::string kSessionKey = "must-never-appear-session-key";
    const std::uint64_t account = 42;
    const std::uint64_t grant = 99887766;
    audit::Record rec =
        build_rate_limited_audit(account, grant, mn::Opcode::CHAT_MESSAGE, RateClass::kChat);
    check("E: action is rate_limited", rec.action == audit::Action::kRateLimited);
    check("E: outcome is failure (warn-level flag)", rec.outcome == audit::Outcome::kFailure);
    check("E: account attributed", rec.account_id == account);
    check("E: grant correlation", rec.correlation_id == grant);
    check("E: target names the offending opcode", rec.target == "opcode:CHAT_MESSAGE");
    check("E: reason is the rate class", rec.reason == "chat");

    // Renders as a warn-severity audit line carrying the stream tag, no secret leak.
    std::string line = audit::render_json(rec);
    check("E: renders on the audit stream", has(line, "\"stream\":\"audit\""));
    check("E: renders the rate_limited action", has(line, "rate_limited"));
    check("E: warn severity", has(line, "\"severity\":\"warn\""));
    check("E: no secret material leaks", !has(line, kSessionKey));
}

}  // namespace

int main() {
    std::printf("worldd per-opcode rate class test (OPS-03b, #421)\n");
    test_table();
    test_chat_flood_vs_compliant();
    test_class_buckets_independent();
    test_window_slides();
    test_audit_record();
    std::printf(g_fail == 0 ? "\nALL RATE CLASS TESTS PASSED\n"
                            : "\n%d RATE CLASS TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
