// SPDX-License-Identifier: Apache-2.0
//
// meridian-items — int64 copper currency (ECO-01; issue #366).
// See currency.h for the two-layer design + the int64/no-FLOAT rationale.

#include "currency.h"

#include <string>

namespace meridian::items {

namespace {

// Bind a 64-bit id (char_id) as a DECIMAL STRING. Same meridian-db gotcha the
// characters layer documents: Connection binds std::int64_t as a SIGNED
// LONGLONG, so a BIGINT UNSIGNED id past INT64_MAX would round-trip wrong. A
// string parameter is coerced by MariaDB into the unsigned column across the full
// range. (Money itself is bound as int64 — it is guaranteed in [0, kMaxCopper].)
db::Param bind_id(std::uint64_t v) { return db::Param{std::to_string(v)}; }

// Parse a money cell (BIGINT UNSIGNED, carried as text) to Copper. The API caps
// balances at kMaxCopper == INT64_MAX, so std::stoll never overflows for a
// balance this code wrote. NULL/empty -> 0 (money is NOT NULL DEFAULT 0, so this
// is defensive only).
Copper parse_money(const db::Cell& c) {
    if (!c.has_value() || c->empty()) return 0;
    return static_cast<Copper>(std::stoll(*c));
}

}  // namespace

Copper checked_add(Copper balance, Copper amount) {
    if (amount < 0) throw NegativeAmount(amount);
    // Overflow test WITHOUT overflowing: kMaxCopper - balance is the headroom.
    if (amount > kMaxCopper - balance) throw CurrencyOverflow(balance, amount);
    return balance + amount;
}

Copper checked_subtract(Copper balance, Copper amount) {
    if (amount < 0) throw NegativeAmount(amount);
    if (amount > balance) throw InsufficientFunds(balance, amount);
    return balance - amount;
}

Copper get_money(db::Connection& conn, std::uint64_t char_id) {
    db::Result r = conn.execute(
        "SELECT money FROM `character` WHERE id = ?", {bind_id(char_id)});
    if (r.rows.empty()) throw CharacterNotFound(char_id);
    return parse_money(r.rows[0][0]);
}

namespace {

// Shared body of add_money/subtract_money: open a transaction, SELECT the
// balance FOR UPDATE (row lock -> the check-then-write is atomic against a
// concurrent economy op), apply `op`, UPDATE, COMMIT. Any throw rolls back so the
// balance is never partially applied. `op` is the pure checked_add/subtract.
template <typename Op>
Copper apply_money(db::Connection& conn, std::uint64_t char_id, Copper amount,
                   Op op) {
    conn.execute("START TRANSACTION");
    try {
        db::Result r = conn.execute(
            "SELECT money FROM `character` WHERE id = ? FOR UPDATE",
            {bind_id(char_id)});
        if (r.rows.empty()) throw CharacterNotFound(char_id);

        const Copper balance = parse_money(r.rows[0][0]);
        const Copper updated = op(balance, amount);  // may throw (checked)

        conn.execute("UPDATE `character` SET money = ? WHERE id = ?",
                     {db::Param{static_cast<std::int64_t>(updated)}, bind_id(char_id)});
        conn.execute("COMMIT");
        return updated;
    } catch (...) {
        conn.execute("ROLLBACK");
        throw;
    }
}

}  // namespace

Copper add_money(db::Connection& conn, std::uint64_t char_id, Copper amount) {
    return apply_money(conn, char_id, amount, checked_add);
}

Copper subtract_money(db::Connection& conn, std::uint64_t char_id, Copper amount) {
    return apply_money(conn, char_id, amount, checked_subtract);
}

}  // namespace meridian::items
