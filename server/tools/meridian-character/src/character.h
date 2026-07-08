// SPDX-License-Identifier: Apache-2.0
//
// meridian-character — character-creation CLI code path (test provisioning /
// the concurrency-harness load driver, mirrors meridian-account, issue #80).
//
// Provenance: composes the existing meridian::characters CRUD
// (server/characters/src/characters.h, CHR-01 stub / D-11) with meridian::db
// (Connection, prepared statements). No new persistence logic is introduced
// here — this library only (a) parses/defaults the CLI's `create` arguments
// and (b) resolves a username to its auth-DB account_id so the CRUD's
// account_id-keyed create_character() can be called. Clean-room, original
// code; no GPL source consulted (CONTRIBUTING.md).
//
// Cross-DB note (server SAD §4.4 soft-ref rule): account_id lives in the auth
// DB (`meridian_auth.account`), the character row lives in the characters DB
// (`meridian_characters`.`character`). There is no FK between them — this
// layer resolves the id with one fully-qualified cross-schema SELECT on the
// SAME connection (the `meridian` DB user has grants on both schemas), then
// hands the plain numeric id to meridian::characters::create_character(),
// which never itself talks to the auth DB (soft-ref rule preserved).
//
// This header exposes the arg-parsing/defaulting and resolve+create code
// paths as small library functions so the test can drive the exact code the
// CLI uses, mirroring meridian-account's account.h/account.cpp split.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "characters.h"
#include "meridian/db/connection.h"

namespace meridian::character_cli {

// --- CLI argument model -------------------------------------------------------

// Parsed `create` arguments (after defaulting). Race/class default to the
// roster.h M0 defaults (kArdent=1, kVanguard=1); name defaults to the
// username (see effective_name()).
struct CreateArgs {
    std::string username;
    std::optional<std::string> name;
    std::uint8_t race = 1;
    std::uint8_t char_class = 1;
};

// DB connection overrides taken from --db-* flags (overlaid onto MERIDIAN_DB_*
// env by build_conn_params()), same shape as meridian-account's DbFlags.
struct DbFlags {
    std::optional<std::string> host, user, pass, name, socket;
    std::optional<unsigned> port;
};

enum class ParseOutcome { kOk, kUsageError };

// Result of parsing `meridian-character create`'s argv (the slice AFTER the
// "create" subcommand token, same convention as meridian-account's
// cmd_create(argc, argv)). On kUsageError, `error` holds a message the caller
// should print to stderr before printing usage; args/db are unspecified.
struct ParseResult {
    ParseOutcome outcome = ParseOutcome::kUsageError;
    CreateArgs args;
    DbFlags db;
    std::string error;
};

// Parse `--username U [--name NAME] [--race R] [--class C] [--db-*...]`.
// --username is the only required flag. --race/--class must parse as an
// integer in [0, 255] (roster-membership, i.e. rejecting 0/5+, is validated
// later by meridian::characters::create_character — this layer only checks
// "is it a u8").
ParseResult parse_create_args(int argc, char** argv);

// The character name to create: args.name if given, else args.username.
std::string effective_name(const CreateArgs& args);

// Build ConnectParams from MERIDIAN_DB_* env, overlaid by --db-* flags (same
// precedence as meridian-account's build_conn_params). Unlike
// meridian-account, when no database name is configured by either the env or
// a flag, this defaults to the characters DB ("meridian_characters") — the
// CRUD's create_character() issues unqualified `character` table queries, so
// the connection's default DB must be the characters schema.
db::ConnectParams build_conn_params(const DbFlags& f);

// Thrown when no auth-DB account matches the given username.
class UnknownAccount : public std::runtime_error {
public:
    explicit UnknownAccount(const std::string& username)
        : std::runtime_error("no account found for username: " + username) {}
};

// Resolve `username` to its auth-DB account id via a fully-qualified,
// cross-schema, parameterized SELECT against `meridian_auth.account` on
// `conn` (conn's own default DB stays the characters DB). Throws
// UnknownAccount if no row matches, meridian::db::DbError on any other DB
// failure.
std::uint64_t resolve_account_id(db::Connection& conn, const std::string& username);

// Result of a successful create-for-account: the resolved account id plus the
// characters CRUD's result (the new character id).
struct CreateForAccountResult {
    std::uint64_t account_id = 0;
    characters::CreateResult character;
};

// Resolve args.username's account_id, then create the character through
// meridian::characters::create_character(). This is the single function the
// CLI and the test both call — mirrors meridian-account's create_account() as
// the one code path exercised by both.
//
// Throws UnknownAccount (no such username), meridian::characters::InvalidName
// / InvalidRace / InvalidClass / DuplicateName / CharacterLimitReached (see
// characters.h), or meridian::db::DbError.
CreateForAccountResult create_character_for_account(db::Connection& conn,
                                                     const CreateArgs& args);

}  // namespace meridian::character_cli
