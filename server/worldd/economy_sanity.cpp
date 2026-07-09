// SPDX-License-Identifier: Apache-2.0
//
// worldd — economy sanity checks implementation (OPS-03b, #421).
// See economy_sanity.h for the provenance + design.

#include "economy_sanity.h"

#include <string>

namespace meridian::worldd {
namespace audit = meridian::core::audit;
namespace log = meridian::core::log;

const char* economy_reject_reason(EconomyReject reject) {
    switch (reject) {
        case EconomyReject::kNone:           return "none";
        case EconomyReject::kBadQuantity:    return "bad_quantity";
        case EconomyReject::kNegativePrice:  return "negative_price";
        case EconomyReject::kMoneyOverflow:  return "money_overflow";
        case EconomyReject::kMoneyUnderflow: return "money_underflow";
    }
    return "none";
}

EconomyReject check_quantity(std::uint32_t quantity) {
    if (quantity == 0 || quantity > kMaxTransactionQuantity) {
        return EconomyReject::kBadQuantity;
    }
    return EconomyReject::kNone;
}

EconomyReject check_amount_nonnegative(std::int64_t amount) {
    return amount < 0 ? EconomyReject::kNegativePrice : EconomyReject::kNone;
}

EconomyReject check_add_money(std::int64_t balance, std::int64_t amount) {
    if (amount < 0) return EconomyReject::kNegativePrice;
    // balance + amount > kMaxCopperBalance, rearranged to avoid the overflow itself.
    if (balance > kMaxCopperBalance - amount) return EconomyReject::kMoneyOverflow;
    return EconomyReject::kNone;
}

EconomyReject check_subtract_money(std::int64_t balance, std::int64_t amount) {
    if (amount < 0) return EconomyReject::kNegativePrice;
    if (amount > balance) return EconomyReject::kMoneyUnderflow;
    return EconomyReject::kNone;
}

core::audit::Record build_economy_reject_audit(std::uint64_t account_id,
                                               std::uint64_t grant_id,
                                               std::uint64_t char_id, const char* path,
                                               EconomyReject reject) {
    audit::Record rec;
    rec.action = audit::Action::kEconomyRejected;
    rec.outcome = audit::Outcome::kFailure;  // an impossible delta is a warn-level flag
    rec.account_id = account_id;             // 0 => omitted
    rec.target = std::string("txn:") + (path != nullptr ? path : "unknown");
    rec.reason = economy_reject_reason(reject);
    rec.correlation_id = grant_id;           // 0 => omitted; pivots audit↔trace↔log
    if (char_id != 0) {
        rec.extra.push_back(
            log::field("char_id", static_cast<std::uint64_t>(char_id)));
    }
    return rec;
}

}  // namespace meridian::worldd
