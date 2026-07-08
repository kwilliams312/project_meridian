// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters — character stub CRUD (CHR-01 stub, D-11; issue #85).
//
// Provenance: designed from the server SAD §4.2 (characters DB `character`
// table — the only durable player state), §4.4 (cross-DB reference rule:
// account_id is a SOFT numeric reference into the auth DB with NO cross-DB FK),
// and sync decision D-11 (docs/01-SYNC-DECISIONS.md — the M0 CHR-01 stub is
// name + class over one placeholder model). Composes meridian-db (Connection +
// prepared statements) against server/db/characters/migrations/0001.
//
// Clean-room, original code; no GPL source consulted (CONTRIBUTING.md).
//
// This header exposes the M0 character-stub CRUD as a library so the enter-world
// / character-select flow (worldd, later a characters service) and the
// integration test can drive the exact same code path:
//   * list_characters(account_id)      — the "list my characters" screen query.
//   * create_character(req)            — validates name + roster, enforces the
//                                        account_id soft-ref rule, INSERTs a row.
//   * delete_character(acct, char_id)  — ownership-scoped delete.
//
// SOFT-REF RULE (§4.4): `account_id` lives in the AUTH DB (account.id); a
// character row lives in the CHARACTERS DB. The reference is numeric only, with
// NO foreign key (a cross-DB FK is impossible in MariaDB and forbidden by §4.4).
// This layer therefore never JOINs to the auth DB — it treats account_id as an
// opaque owner token: create() stamps it onto the row, list()/delete() filter by
// it. Ownership is enforced entirely by the account_id predicate on the query.
//
// UNSIGNED-ID BINDING (meridian-db gotcha): character.id and character.account_id
// are BIGINT UNSIGNED, but meridian-db binds std::int64_t as a SIGNED LONGLONG
// (connection.cpp does not set is_unsigned). Binding an id > INT64_MAX through
// the int64 path would round-trip wrong. So every 64-bit id here binds as a
// DECIMAL STRING (std::to_string) — MariaDB coerces the string parameter into
// the BIGINT UNSIGNED column across the full unsigned range — and reads back with
// std::stoull. See characters.cpp (bind_u64 / parse_u64).

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "meridian/db/connection.h"
#include "roster.h"

namespace meridian::characters {

// One character as shown on the character-select screen (list result row).
struct CharacterSummary {
    std::uint64_t id = 0;          // character.id (server-minted, BIGINT UNSIGNED)
    std::uint64_t account_id = 0;  // owning account (soft ref -> auth DB, §4.4)
    std::string name;              // character.name (unique, case-insensitive)
    std::uint8_t race = 0;         // M0-frozen race id (roster.h)
    std::uint8_t char_class = 0;   // M0-frozen class id (roster.h)
    std::uint16_t level = 0;       // character.level
};

// Parameters for one character-create request (CHR-01 stub / D-11 fields).
struct CreateRequest {
    std::uint64_t account_id = 0;  // owner (soft ref -> auth DB account.id, §4.4)
    std::string name;              // desired character name
    std::uint8_t race = 0;         // M0-frozen race id (validated against roster)
    std::uint8_t char_class = 0;   // M0-frozen class id (validated against roster)
};

// Result of a successful create: the server-minted character id.
struct CreateResult {
    std::uint64_t character_id = 0;
};

// --- Errors ------------------------------------------------------------------
// create() throws exactly one of these on a rejected request; the caller (a
// service or the CLI/test) maps each to a protocol/exit code.

// Name is empty or exceeds the schema's VARCHAR(32).
class InvalidName : public std::invalid_argument {
public:
    explicit InvalidName(const std::string& why)
        : std::invalid_argument("invalid character name: " + why) {}
};

// race is not a defined M0-frozen race id (roster.h is_valid_race).
class InvalidRace : public std::invalid_argument {
public:
    explicit InvalidRace(std::uint8_t race)
        : std::invalid_argument("invalid race id: " + std::to_string(race)) {}
};

// char_class is not a defined M0-frozen class id (roster.h is_valid_class).
class InvalidClass : public std::invalid_argument {
public:
    explicit InvalidClass(std::uint8_t cls)
        : std::invalid_argument("invalid class id: " + std::to_string(cls)) {}
};

// Name already taken (characters DB uq_character_name; case-insensitive).
class DuplicateName : public std::runtime_error {
public:
    explicit DuplicateName(const std::string& name)
        : std::runtime_error("character name already exists: " + name) {}
};

// Maximum number of characters a single account may own (issue #329). M0.5 caps
// this at 1 for concurrency testing — one account controls one character at a
// time. It is a NAMED constant (not a magic 1) precisely so it can be raised
// later without touching the enforcement logic. Enforced server-side inside the
// create transaction (characters.cpp) so the check-then-insert cannot race.
inline constexpr std::uint64_t kMaxCharactersPerAccount = 1;

// Account is already at kMaxCharactersPerAccount (issue #329). A create is
// refused server-side — mirrors DuplicateName as a typed, machine-readable
// REFUSED reason the caller maps onto its protocol status (LIMIT_REACHED).
class CharacterLimitReached : public std::runtime_error {
public:
    explicit CharacterLimitReached(std::uint64_t account_id)
        : std::runtime_error(
              "account " + std::to_string(account_id) +
              " already owns the maximum of " +
              std::to_string(kMaxCharactersPerAccount) + " character(s)") {}
};

// Maximum character name length — mirrors character.name VARCHAR(32) so an
// over-long name is rejected here with a clear error instead of a DB truncation.
inline constexpr std::size_t kMaxNameLen = 32;

// --- CRUD --------------------------------------------------------------------

// List every character owned by `account_id`, ordered by id (creation order).
// Returns an empty vector when the account has no characters. Never JOINs the
// auth DB (soft-ref rule); `account_id` is filtered as an opaque owner token.
std::vector<CharacterSummary> list_characters(db::Connection& conn,
                                              std::uint64_t account_id);

// Create a character for `req.account_id`. Validation order:
//   1. name non-empty and <= kMaxNameLen               -> InvalidName
//   2. race in the M0-frozen roster (roster.h)          -> InvalidRace
//   3. class in the M0-frozen roster (roster.h)         -> InvalidClass
//   4. account below kMaxCharactersPerAccount (#329)     -> CharacterLimitReached
//   5. name unique (DB uq_character_name, case-insens.) -> DuplicateName
// Steps 4+5 run together in ONE DB transaction (a locking count then the INSERT)
// so the per-account cap cannot be beaten by a check-then-insert race between two
// concurrent creates. On success INSERTs the row (stamping account_id + the M0
// start location) and returns the server-minted character id. Throws
// meridian::db::DbError on any other DB failure.
CreateResult create_character(db::Connection& conn, const CreateRequest& req);

// Delete character `character_id`, but ONLY if it is owned by `account_id`.
// The ownership check is the query itself — `WHERE id = ? AND account_id = ?` —
// so another account's character (or a non-existent id) deletes nothing.
// Returns true iff a row was deleted (affected_rows == 1), false otherwise.
bool delete_character(db::Connection& conn, std::uint64_t account_id,
                      std::uint64_t character_id);

}  // namespace meridian::characters
