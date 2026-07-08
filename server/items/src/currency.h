// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — int64 copper currency (ECO-01; server PRD §4-M1 "money as
// int64 copper"; issue #366).
//
// Two layers, both here:
//   1. PURE, DB-free checked arithmetic (checked_add / checked_subtract) — the
//      invariant core: a balance is a whole number of copper in [0, kMaxCopper],
//      NEVER negative and NEVER a FLOAT. Deterministically unit-tested (no DB).
//   2. DB-backed operations on `character.money` (add_money / subtract_money /
//      get_money) — each a SINGLE characters-DB transaction (SELECT ... FOR
//      UPDATE, checked apply, UPDATE), matching the SAD §4.7 rule that economy
//      operations are single-transaction and never batched. Parameterized SQL
//      only (meridian-db prepared statements).
//
// WHY int64 and not FLOAT: currency must be exact — floating point silently loses
// low copper on large balances. WHY signed int64 and not the column's unsigned
// range: the PRD fixes the API type as int64 copper; the API caps balances at
// kMaxCopper (== INT64_MAX) so every legal balance fits a signed int64 exactly
// and the checked ops can reason about overflow with ordinary signed math.
//
// Clean-room, original code (CONTRIBUTING.md).

#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "item_template.h"  // Copper
#include "meridian/db/connection.h"

namespace meridian::items {

// The maximum balance the economy will hold. INT64_MAX copper is astronomically
// larger than any reachable balance, so this is a safety ceiling (overflow guard)
// rather than a design cap. Balances are always in [0, kMaxCopper].
inline constexpr Copper kMaxCopper = std::numeric_limits<Copper>::max();

// --- Errors ------------------------------------------------------------------

// A subtract would drive the balance below zero (the caller cannot afford it).
// Carries the balance and the requested amount for the caller's protocol reply.
class InsufficientFunds : public std::runtime_error {
public:
    InsufficientFunds(Copper balance, Copper amount)
        : std::runtime_error("insufficient funds: balance " +
                             std::to_string(balance) + " < requested " +
                             std::to_string(amount)),
          balance_(balance), amount_(amount) {}
    Copper balance() const { return balance_; }
    Copper amount() const { return amount_; }

private:
    Copper balance_;
    Copper amount_;
};

// An add would push the balance past kMaxCopper (overflow). Effectively
// unreachable in normal play; guards against a runaway/exploit add.
class CurrencyOverflow : public std::runtime_error {
public:
    CurrencyOverflow(Copper balance, Copper amount)
        : std::runtime_error("currency overflow: balance " +
                             std::to_string(balance) + " + " +
                             std::to_string(amount) + " exceeds max"),
          balance_(balance), amount_(amount) {}
    Copper balance() const { return balance_; }
    Copper amount() const { return amount_; }

private:
    Copper balance_;
    Copper amount_;
};

// An amount passed to add/subtract was negative. The API is explicit: use
// subtract to remove money, add to grant it — a negative amount is a caller bug,
// never a silent sign flip.
class NegativeAmount : public std::invalid_argument {
public:
    explicit NegativeAmount(Copper amount)
        : std::invalid_argument("currency amount must be non-negative: " +
                               std::to_string(amount)),
          amount_(amount) {}
    Copper amount() const { return amount_; }

private:
    Copper amount_;
};

// No `character` row for the id (a subtract/add against a missing character).
class CharacterNotFound : public std::runtime_error {
public:
    explicit CharacterNotFound(std::uint64_t char_id)
        : std::runtime_error("no character row for id " + std::to_string(char_id)),
          char_id_(char_id) {}
    std::uint64_t char_id() const { return char_id_; }

private:
    std::uint64_t char_id_;
};

// --- Pure checked arithmetic (no DB) -----------------------------------------

// balance + amount, guarding both invariants. Throws NegativeAmount if amount<0
// and CurrencyOverflow if the sum would exceed kMaxCopper. `balance` is assumed
// already valid ([0, kMaxCopper]).
Copper checked_add(Copper balance, Copper amount);

// balance - amount, guarding both invariants. Throws NegativeAmount if amount<0
// and InsufficientFunds if amount > balance (the result would go negative). Never
// returns a value below zero.
Copper checked_subtract(Copper balance, Copper amount);

// --- DB-backed money operations on `character.money` -------------------------

// Read the current balance for `char_id`. Throws CharacterNotFound if the row is
// absent, meridian::db::DbError on a DB failure.
Copper get_money(db::Connection& conn, std::uint64_t char_id);

// Add `amount` copper to `char_id` in ONE transaction (SELECT ... FOR UPDATE,
// checked_add, UPDATE). Returns the new balance. Throws NegativeAmount,
// CurrencyOverflow, CharacterNotFound, or db::DbError — the transaction is rolled
// back on any throw so the balance is never left partially applied.
Copper add_money(db::Connection& conn, std::uint64_t char_id, Copper amount);

// Subtract `amount` copper from `char_id` in ONE transaction. Returns the new
// balance. Throws InsufficientFunds if the character cannot afford it (balance
// unchanged), plus the same errors as add_money. The FOR UPDATE row lock makes
// the check-then-write atomic against a concurrent spend.
Copper subtract_money(db::Connection& conn, std::uint64_t char_id, Copper amount);

}  // namespace meridian::items
