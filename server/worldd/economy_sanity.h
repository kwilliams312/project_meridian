// SPDX-License-Identifier: Apache-2.0
//
// worldd — ECONOMY SANITY checks (OPS-03b, #421; epic #21).
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md §4-M1 (OPS-03 "economy sanity
// checks") + §6 ("append-only audit stream for economy transactions"),
// docs/sad/server-sad.md §4.7 (economy operations are single-transaction), the
// ECO-01 int64-copper currency invariants (server/items/src/currency.h, #366), and
// the OPS-05 audit facility (meridian/core/audit.hpp, #92). No GPL source
// consulted. See CONTRIBUTING.md.
//
// WHAT THIS IS. A thin, PURE, defense-in-depth layer the vendor / loot / quest-
// reward handlers run BEFORE they touch durable money+item state, rejecting any
// transaction whose delta is IMPOSSIBLE: a bad quantity (zero or absurdly large), a
// negative price/credit, or a copper add/subtract that would over/underflow the
// int64 balance. It COMPLEMENTS — never replaces — the per-feature validation that
// already lives deeper (vendor's BadStackCount, currency's checked_add/subtract
// which are the AUTHORITATIVE enforcement under the row lock). This layer is the
// belt to that suspenders: a fast, catalog-independent pre-check at the wire
// boundary that also AUDITS the rejection (action="economy_rejected"), so an
// impossible delta is caught, refused, and flagged before any DB round-trip.
//
// PURITY. Every check is a pure function of its inputs (no DB, no socket, no clock),
// so the whole layer is deterministically unit-testable with no MariaDB — and the
// same invariant helpers can guard a server-DERIVED value (a reward-money credit,
// a loot copper pile) as easily as a client-SUPPLIED one (a buy/sell quantity).
//
// NO SECRETS (#92). The audit record carries identity + the transaction path + the
// reject classification ONLY — never a balance-revealing oracle beyond what the
// client already knows, and never any secret material.

#ifndef MERIDIAN_WORLDD_ECONOMY_SANITY_H
#define MERIDIAN_WORLDD_ECONOMY_SANITY_H

#include <cstdint>

#include "meridian/core/audit.hpp"

namespace meridian::worldd {

// The largest quantity a single economy transaction may name. A legitimate stack is
// bounded by the item template's max_stack (validated deeper); this is a coarse,
// template-independent ceiling that rejects a nonsense quantity (e.g. a 32-bit
// overflow probe) at the wire boundary before any price arithmetic. Generous enough
// to never clip a real bulk buy/sell.
inline constexpr std::uint32_t kMaxTransactionQuantity = 1000;

// The maximum copper balance the economy holds (== items::kMaxCopper == INT64_MAX).
// Duplicated as a plain constant here so this header has NO dependency on the items
// library (keeps the pure layer link-light + independently testable); the currency
// layer's checked_add is the authoritative overflow guard at the DB.
inline constexpr std::int64_t kMaxCopperBalance = INT64_MAX;

// Why an economy transaction was rejected by the sanity layer (server-side detail;
// the wire reply is the feature's existing typed status — e.g. VendorBuyStatus::
// BAD_QUANTITY — so the client sees no new oracle).
enum class EconomyReject : std::uint8_t {
    kNone = 0,        // the transaction's delta is sane
    kBadQuantity,     // quantity is zero or exceeds kMaxTransactionQuantity
    kNegativePrice,   // a price / credit / copper amount is negative
    kMoneyOverflow,   // an ADD would push the balance past kMaxCopperBalance
    kMoneyUnderflow,  // a SUBTRACT would drive the balance below zero
};

// The stable, low-cardinality `reason` string for a reject (the audit / metric
// classification): "bad_quantity" / "negative_price" / "money_overflow" /
// "money_underflow" ("none" when not a reject).
const char* economy_reject_reason(EconomyReject reject);

// --- Pure delta checks (no DB) ----------------------------------------------

// A quantity is sane iff it is in [1, kMaxTransactionQuantity]. Returns kNone when
// sane, else kBadQuantity.
EconomyReject check_quantity(std::uint32_t quantity);

// A price / credit / copper amount is sane iff it is >= 0 (money never flows in a
// negative amount — the API is add-to-grant / subtract-to-remove, never a sign
// flip). Returns kNone when sane, else kNegativePrice.
EconomyReject check_amount_nonnegative(std::int64_t amount);

// Adding `amount` copper to `balance` (both assumed in [0, kMaxCopperBalance]) is
// sane iff `amount` >= 0 and the sum does not exceed kMaxCopperBalance. Returns
// kNone, kNegativePrice, or kMoneyOverflow.
EconomyReject check_add_money(std::int64_t balance, std::int64_t amount);

// Subtracting `amount` copper from `balance` is sane iff `amount` >= 0 and
// `amount` <= `balance` (the result never goes negative). Returns kNone,
// kNegativePrice, or kMoneyUnderflow.
EconomyReject check_subtract_money(std::int64_t balance, std::int64_t amount);

// --- Audit ------------------------------------------------------------------

// Build the append-only ECONOMY audit record for one rejected transaction (server
// PRD §6). `account_id`/`grant_id` attribute + correlate the flag (0 => omitted);
// `char_id` is the acting character (0 => omitted from `extra`); `path` is the
// transaction path ("vendor_buy" / "vendor_sell" / "quest_reward" / "loot_money");
// `reject` classifies the impossible delta. PURE — the dispatch emits the result
// via core::audit::emit.
core::audit::Record build_economy_reject_audit(std::uint64_t account_id,
                                               std::uint64_t grant_id,
                                               std::uint64_t char_id, const char* path,
                                               EconomyReject reject);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_ECONOMY_SANITY_H
