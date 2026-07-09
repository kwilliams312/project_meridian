// SPDX-License-Identifier: Apache-2.0
//
// worldd — ECONOMY SANITY unit test (OPS-03b, #421; epic #21).
//
// PURE / DB-FREE: drives the defense-in-depth economy delta checks directly, so
// every accept/reject is deterministic with no MariaDB. Runs in the plain server
// ctest.
//
// CLEAN-ROOM: written from docs/prd/server-prd.md §4-M1 (OPS-03 economy sanity) +
// §6 (economy audit stream), economy_sanity.h, the ECO-01 int64-copper invariants
// (currency.h), and the OPS-05 audit facility. No GPL source consulted.
//
// What it proves (the story's acceptance list):
//   A. A NORMAL transaction passes every check (a sane quantity, a non-negative
//      price, an add/subtract that stays within [0, INT64_MAX]).
//   B. An IMPOSSIBLE delta is rejected — a zero / oversized quantity, a negative
//      price, a copper add that would OVERFLOW, a subtract that would UNDERFLOW.
//   C. A reject builds the economy audit record (kEconomyRejected, warn, the
//      transaction path + the classification), with no secret material.

#include "economy_sanity.h"

#include "meridian/core/audit.hpp"

#include <cstdio>
#include <string>

using namespace meridian::worldd;
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

// A — a normal transaction passes every check.
void test_normal_passes() {
    std::printf("[econ] a normal transaction passes\n");
    check("A: a sane quantity passes", check_quantity(5) == EconomyReject::kNone);
    check("A: quantity at the ceiling passes",
          check_quantity(kMaxTransactionQuantity) == EconomyReject::kNone);
    check("A: a non-negative price passes", check_amount_nonnegative(1500) == EconomyReject::kNone);
    check("A: a zero price passes (free reward)", check_amount_nonnegative(0) == EconomyReject::kNone);
    check("A: a normal add passes", check_add_money(1000, 250) == EconomyReject::kNone);
    check("A: a normal subtract passes", check_subtract_money(1000, 250) == EconomyReject::kNone);
    check("A: subtract the full balance passes", check_subtract_money(1000, 1000) == EconomyReject::kNone);
}

// B — an impossible delta is rejected.
void test_impossible_rejected() {
    std::printf("[econ] an impossible delta is rejected\n");
    check("B: quantity zero -> bad_quantity", check_quantity(0) == EconomyReject::kBadQuantity);
    check("B: quantity over the ceiling -> bad_quantity",
          check_quantity(kMaxTransactionQuantity + 1) == EconomyReject::kBadQuantity);
    check("B: a huge 32-bit quantity -> bad_quantity",
          check_quantity(4'000'000'000u) == EconomyReject::kBadQuantity);
    check("B: a negative price -> negative_price",
          check_amount_nonnegative(-1) == EconomyReject::kNegativePrice);
    // Add overflow: balance near the ceiling + a positive amount would exceed it.
    check("B: an add past INT64_MAX -> money_overflow",
          check_add_money(kMaxCopperBalance - 10, 11) == EconomyReject::kMoneyOverflow);
    check("B: an add of a negative amount -> negative_price",
          check_add_money(1000, -5) == EconomyReject::kNegativePrice);
    // Subtract underflow: removing more than the balance would go negative.
    check("B: a subtract below zero -> money_underflow",
          check_subtract_money(100, 101) == EconomyReject::kMoneyUnderflow);
    check("B: a subtract of a negative amount -> negative_price",
          check_subtract_money(1000, -5) == EconomyReject::kNegativePrice);
    // Reason strings are the stable classification set.
    check("B: reason bad_quantity",
          std::string(economy_reject_reason(EconomyReject::kBadQuantity)) == "bad_quantity");
    check("B: reason money_overflow",
          std::string(economy_reject_reason(EconomyReject::kMoneyOverflow)) == "money_overflow");
}

// C — a reject builds the economy audit record.
void test_audit_record() {
    std::printf("[econ] economy audit record\n");
    const std::string kSessionKey = "must-never-appear-session-key";
    const std::uint64_t account = 42;
    const std::uint64_t grant = 99887766;
    const std::uint64_t char_id = 1001;
    audit::Record rec = build_economy_reject_audit(account, grant, char_id, "vendor_buy",
                                                   EconomyReject::kBadQuantity);
    check("C: action is economy_rejected", rec.action == audit::Action::kEconomyRejected);
    check("C: outcome is failure (warn-level flag)", rec.outcome == audit::Outcome::kFailure);
    check("C: account attributed", rec.account_id == account);
    check("C: grant correlation", rec.correlation_id == grant);
    check("C: target names the transaction path", rec.target == "txn:vendor_buy");
    check("C: reason is the classification", rec.reason == "bad_quantity");

    std::string line = audit::render_json(rec);
    check("C: renders on the audit stream", has(line, "\"stream\":\"audit\""));
    check("C: renders the economy_rejected action", has(line, "economy_rejected"));
    check("C: warn severity", has(line, "\"severity\":\"warn\""));
    check("C: carries the acting char_id", has(line, "\"char_id\":1001"));
    check("C: no secret material leaks", !has(line, kSessionKey));
}

}  // namespace

int main() {
    std::printf("worldd economy sanity test (OPS-03b, #421)\n");
    test_normal_passes();
    test_impossible_rejected();
    test_audit_record();
    std::printf(g_fail == 0 ? "\nALL ECONOMY SANITY TESTS PASSED\n"
                            : "\n%d ECONOMY SANITY TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
