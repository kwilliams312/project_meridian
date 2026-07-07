// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters — character stub CRUD (CHR-01 stub, D-11; issue #85).
// See characters.h for provenance / the soft-ref + unsigned-binding rationale.

#include "characters.h"

#include <stdexcept>
#include <string>

namespace meridian::characters {

namespace {

// MariaDB / MySQL error number for a duplicate entry on a UNIQUE key
// (ER_DUP_ENTRY — MariaDB error reference, not GPL source). Same constant the
// account-creation path uses for its username UNIQUE key.
constexpr unsigned kErDupEntry = 1062;

// M0 start location (CHR-01 stub). The M0 world is the single flat test map
// (docs/it-m0-runbook.md — "flat test map"); a new stub character spawns at its
// origin. These are deliberate M0 placeholders: real per-race start locations
// arrive with world content. character.level defaults to 1 and money/xp to 0 in
// the schema, so only the NOT-NULL-without-default columns are supplied here.
constexpr std::uint32_t kM0StartMapId = 0;  // flat test map (world DB map id 0)
constexpr float kM0StartX = 0.0F;
constexpr float kM0StartY = 0.0F;
constexpr float kM0StartZ = 0.0F;

// Bind a 64-bit UNSIGNED id as a DECIMAL STRING. meridian-db binds std::int64_t
// as a SIGNED LONGLONG (connection.cpp sets no is_unsigned flag), so an id past
// INT64_MAX would round-trip incorrectly through the int path. A string param is
// coerced by MariaDB into the BIGINT UNSIGNED column across the full unsigned
// range — this is the characters.h "unsigned-id binding" rule in one place.
db::Param bind_u64(std::uint64_t v) { return db::Param{std::to_string(v)}; }

// Parse a BIGINT UNSIGNED result cell (carried as text by meridian-db) back to
// a uint64. Empty/NULL -> 0. Symmetric with bind_u64.
std::uint64_t parse_u64(const db::Cell& c) {
    if (!c.has_value() || c->empty()) return 0;
    return std::stoull(*c);
}

// Parse a small unsigned numeric cell (race/class/level) to uint. NULL -> 0.
unsigned parse_uint(const db::Cell& c) {
    if (!c.has_value() || c->empty()) return 0;
    return static_cast<unsigned>(std::stoul(*c));
}

}  // namespace

std::vector<CharacterSummary> list_characters(db::Connection& conn,
                                              std::uint64_t account_id) {
    // Soft-ref rule (§4.4): filter by the numeric account_id only — no JOIN to
    // the auth DB. account_id binds as a decimal string (BIGINT UNSIGNED).
    db::Result r = conn.execute(
        "SELECT id, account_id, name, race, class, level "
        "FROM `character` WHERE account_id = ? ORDER BY id",
        {bind_u64(account_id)});

    std::vector<CharacterSummary> out;
    out.reserve(r.rows.size());
    for (const db::Row& row : r.rows) {
        CharacterSummary c;
        c.id = parse_u64(row[0]);
        c.account_id = parse_u64(row[1]);
        c.name = row[2].value_or("");
        c.race = static_cast<std::uint8_t>(parse_uint(row[3]));
        c.char_class = static_cast<std::uint8_t>(parse_uint(row[4]));
        c.level = static_cast<std::uint16_t>(parse_uint(row[5]));
        out.push_back(std::move(c));
    }
    return out;
}

CreateResult create_character(db::Connection& conn, const CreateRequest& req) {
    // 1. Name: non-empty, within the schema's VARCHAR(32).
    if (req.name.empty()) {
        throw InvalidName("must not be empty");
    }
    if (req.name.size() > kMaxNameLen) {
        throw InvalidName("exceeds " + std::to_string(kMaxNameLen) + " characters");
    }
    // 2/3. Race + class: must be in the M0-frozen roster (roster.h).
    if (!is_valid_race(req.race)) {
        throw InvalidRace(req.race);
    }
    if (!is_valid_class(req.char_class)) {
        throw InvalidClass(req.char_class);
    }

    // 4. INSERT. name uniqueness (case-insensitive) is enforced by the DB
    // UNIQUE key uq_character_name -> ER_DUP_ENTRY -> DuplicateName. Every value
    // binds through a placeholder (never concatenated); ids bind as decimal
    // strings (unsigned-id rule). Only NOT-NULL-without-default columns are set;
    // level/xp/money/pos_o/save_epoch/timestamps take their schema defaults.
    std::vector<db::Param> bind{
        bind_u64(req.account_id),                      // account_id (soft ref)
        db::Param{req.name},                           // name
        db::Param{static_cast<std::int64_t>(req.race)},        // race
        db::Param{static_cast<std::int64_t>(req.char_class)},  // class
        db::Param{static_cast<std::int64_t>(kM0StartMapId)},   // map_id
        db::Param{static_cast<double>(kM0StartX)},     // pos_x
        db::Param{static_cast<double>(kM0StartY)},     // pos_y
        db::Param{static_cast<double>(kM0StartZ)},     // pos_z
    };

    try {
        db::Result r = conn.execute(
            "INSERT INTO `character` "
            "(account_id, name, race, class, map_id, pos_x, pos_y, pos_z) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            bind);
        return CreateResult{r.last_insert_id};
    } catch (const db::DbError& e) {
        if (e.code() == kErDupEntry) {
            throw DuplicateName(req.name);
        }
        throw;
    }
}

bool delete_character(db::Connection& conn, std::uint64_t account_id,
                      std::uint64_t character_id) {
    // Ownership is the query: the account_id predicate means another account's
    // character (or a missing id) matches zero rows and deletes nothing. The
    // schema's ON DELETE CASCADE fans the delete out to the child tables
    // (inventory, quests, ...) — not exercised by the M0 stub but relied upon.
    db::Result r = conn.execute(
        "DELETE FROM `character` WHERE id = ? AND account_id = ?",
        {bind_u64(character_id), bind_u64(account_id)});
    return r.affected_rows == 1;
}

}  // namespace meridian::characters
