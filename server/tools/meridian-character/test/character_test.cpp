// SPDX-License-Identifier: Apache-2.0
//
// meridian-character test.
//
// Two sections:
//   1. Unit tests for the arg-parsing/defaulting code path
//      (character_cli::parse_create_args / effective_name / build_conn_params)
//      — DB-free, always run.
//   2. An integration section exercising resolve_account_id() +
//      create_character_for_account() against a live MariaDB. It needs both
//      the meridian_auth and meridian_characters schemas loaded; it reads
//      MERIDIAN_DB_* env (same var names as meridian-account's test) and
//      SKIPS (exit 0) when none are set, so it is inert in the plain server
//      build's ctest and runs for real only with env set (or a dedicated CI
//      job) — same pattern as meridian-account-test / meridian-characters-test.
//
// Clean-room, original code; no GPL source consulted (CONTRIBUTING.md).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "character.h"
#include "meridian/db/connection.h"

using namespace meridian;

namespace {
int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Build a char*[] argv slice (post-subcommand) from string literals, the way
// main() hands argv+2 to parse_create_args.
std::vector<char*> make_argv(std::vector<std::string>& storage) {
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& s : storage) argv.push_back(s.data());
    return argv;
}

// Run a block with the six MERIDIAN_DB_* vars unset, then restore whatever
// was there before. Isolates build_conn_params()'s "default to the
// characters DB" behavior from whatever env the test process inherited.
template <typename Fn>
void with_clean_db_env(Fn&& fn) {
    static const char* kVars[] = {"MERIDIAN_DB_SOCKET", "MERIDIAN_DB_HOST",
                                   "MERIDIAN_DB_PORT",   "MERIDIAN_DB_USER",
                                   "MERIDIAN_DB_PASS",   "MERIDIAN_DB_NAME"};
    std::vector<std::optional<std::string>> saved;
    for (const char* v : kVars) {
        const char* cur = std::getenv(v);
        saved.emplace_back(cur ? std::optional<std::string>(cur) : std::nullopt);
        ::unsetenv(v);
    }
    fn();
    for (std::size_t i = 0; i < std::size(kVars); ++i) {
        if (saved[i]) ::setenv(kVars[i], saved[i]->c_str(), 1);
        else ::unsetenv(kVars[i]);
    }
}

void run_unit_tests() {
    using namespace character_cli;

    std::printf("-- arg parsing --\n");

    // --username is required.
    {
        std::vector<std::string> raw = {"--name", "Foo"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("missing --username is a usage error", r.outcome == ParseOutcome::kUsageError);
    }

    // Minimal valid invocation: only --username.
    {
        std::vector<std::string> raw = {"--username", "alice"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("--username alone parses ok", r.outcome == ParseOutcome::kOk);
        check("username captured", r.args.username == "alice");
        check("name defaults to unset", !r.args.name.has_value());
        check("race defaults to 1 (Ardent)", r.args.race == 1);
        check("class defaults to 1 (Vanguard)", r.args.char_class == 1);
        check("effective_name() defaults to username",
              effective_name(r.args) == "alice");
    }

    // --name overrides the default.
    {
        std::vector<std::string> raw = {"--username", "alice", "--name", "Aeloria"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("--name parses ok", r.outcome == ParseOutcome::kOk);
        check("effective_name() uses --name when given",
              effective_name(r.args) == "Aeloria");
    }

    // --race / --class parse as u8.
    {
        std::vector<std::string> raw = {"--username", "bob", "--race", "3", "--class", "2"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("--race/--class parse ok", r.outcome == ParseOutcome::kOk);
        check("race captured", r.args.race == 3);
        check("class captured", r.args.char_class == 2);
    }

    // Out-of-range / non-numeric --race is a usage error (not silently clamped).
    {
        std::vector<std::string> raw = {"--username", "bob", "--race", "not-a-number"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("non-numeric --race is a usage error", r.outcome == ParseOutcome::kUsageError);
    }
    {
        std::vector<std::string> raw = {"--username", "bob", "--race", "999"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("out-of-u8-range --race is a usage error", r.outcome == ParseOutcome::kUsageError);
    }

    // Unknown flag is a usage error.
    {
        std::vector<std::string> raw = {"--username", "bob", "--wat", "x"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("unknown flag is a usage error", r.outcome == ParseOutcome::kUsageError);
    }

    // --db-* flags flow into ParseResult.db.
    {
        std::vector<std::string> raw = {"--username", "bob", "--db-host", "10.0.0.5",
                                         "--db-port", "3307", "--db-name", "meridian_characters"};
        auto argv = make_argv(raw);
        ParseResult r = parse_create_args(static_cast<int>(argv.size()), argv.data());
        check("--db-* flags parse ok", r.outcome == ParseOutcome::kOk);
        check("db.host captured", r.db.host.has_value() && *r.db.host == "10.0.0.5");
        check("db.port captured", r.db.port.has_value() && *r.db.port == 3307U);
        check("db.name captured", r.db.name.has_value() && *r.db.name == "meridian_characters");
    }

    std::printf("-- build_conn_params --\n");

    with_clean_db_env([] {
        DbFlags empty;
        db::ConnectParams cp = build_conn_params(empty);
        check("defaults database to meridian_characters when unset",
              cp.database == "meridian_characters");

        DbFlags overridden;
        overridden.name = "some_other_db";
        db::ConnectParams cp2 = build_conn_params(overridden);
        check("--db-name overrides the characters-DB default",
              cp2.database == "some_other_db");
    });
}

// Minimal DDL, mirroring meridian-account-test / meridian-characters-test's
// approach: created if absent, own rows cleaned up, never drops a table it
// did not create.
constexpr const char* kAccountDdl =
    "CREATE TABLE IF NOT EXISTS account ("
    "  id            BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,"
    "  username      VARCHAR(32)      NOT NULL,"
    "  srp_salt      VARBINARY(32)    NOT NULL,"
    "  srp_verifier  VARBINARY(256)   NOT NULL,"
    "  gm_level      TINYINT UNSIGNED NOT NULL DEFAULT 0,"
    "  email         VARCHAR(254)     NULL,"
    "  locked        TINYINT(1)       NOT NULL DEFAULT 0,"
    "  created_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  last_login    DATETIME         NULL,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_account_username (username)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id            BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,"
    "  account_id    BIGINT UNSIGNED  NOT NULL,"
    "  name          VARCHAR(32)      NOT NULL,"
    "  race          TINYINT UNSIGNED NOT NULL,"
    "  class         TINYINT UNSIGNED NOT NULL,"
    "  level         SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  map_id        INT UNSIGNED     NOT NULL DEFAULT 0,"
    "  pos_x         FLOAT            NOT NULL DEFAULT 0,"
    "  pos_y         FLOAT            NOT NULL DEFAULT 0,"
    "  pos_z         FLOAT            NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_name (name),"
    "  KEY ix_character_account_id (account_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

int run_integration_tests() {
    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured (set "
                    "MERIDIAN_DB_SOCKET or MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        return 0;
    }

    const std::string username = "chr_test_" + std::to_string(std::rand());
    const std::string charname = "ChrTest" + std::to_string(std::rand());

    try {
        // Auth-DB connection to seed a throwaway account row.
        db::ConnectParams auth_p = p;
        auth_p.database = "meridian_auth";
        db::Connection auth_db(auth_p);
        auth_db.execute(kAccountDdl);
        auth_db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db::Result ins = auth_db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier, gm_level, email) "
            "VALUES (?, ?, ?, 0, NULL)",
            {db::Param{username}, db::Param{db::Bytes_t{1, 2, 3}}, db::Param{db::Bytes_t{4, 5, 6}}});
        std::uint64_t seeded_account_id = ins.last_insert_id;
        check("seeded auth account", seeded_account_id > 0);

        // Characters-DB connection — the one the CLI actually uses.
        db::ConnectParams char_p = p;
        char_p.database = "meridian_characters";
        db::Connection char_db(char_p);
        char_db.execute(kCharacterDdl);
        char_db.execute("DELETE FROM `character` WHERE account_id = ?",
                        {db::Param{std::to_string(seeded_account_id)}});

        std::printf("-- resolve_account_id / create_character_for_account --\n");

        std::uint64_t resolved = character_cli::resolve_account_id(char_db, username);
        check("resolve_account_id finds the seeded account",
              resolved == seeded_account_id);

        bool threw_unknown = false;
        try {
            character_cli::resolve_account_id(char_db, username + "_nope");
        } catch (const character_cli::UnknownAccount&) {
            threw_unknown = true;
        }
        check("resolve_account_id throws UnknownAccount for a missing username",
              threw_unknown);

        character_cli::CreateArgs args;
        args.username = username;
        args.name = charname;
        args.race = 1;
        args.char_class = 1;
        character_cli::CreateForAccountResult created =
            character_cli::create_character_for_account(char_db, args);
        check("account_id resolved on the create path", created.account_id == seeded_account_id);
        check("character id assigned", created.character.character_id > 0);

        // #329 cap (raised to 8 in #629) — the account can fill up to
        // kMaxCharactersPerAccount, then one more create is refused. Names must
        // be globally unique, so each fill uses a distinct name. Driven off the
        // constant so the test tracks any future cap retune. `charname` above is
        // the account's 1st character.
        bool filled = true;
        for (std::uint64_t i = 1; i < characters::kMaxCharactersPerAccount; ++i) {
            character_cli::CreateArgs fill = args;
            fill.name = charname + "_" + std::to_string(i);
            character_cli::CreateForAccountResult rf =
                character_cli::create_character_for_account(char_db, fill);
            filled = filled && rf.character.character_id > 0;
        }
        check("the account fills up to kMaxCharactersPerAccount characters", filled);

        bool threw_limit = false;
        character_cli::CreateArgs over = args;
        over.name = charname + "_over";
        try {
            character_cli::create_character_for_account(char_db, over);
        } catch (const characters::CharacterLimitReached&) {
            threw_limit = true;
        }
        check("a create past kMaxCharactersPerAccount hits the #329 cap",
              threw_limit);

        // Clean up.
        char_db.execute("DELETE FROM `character` WHERE account_id = ?",
                        {db::Param{std::to_string(seeded_account_id)}});
        auth_db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }
    return 1;  // ran (vs. SKIP's early return 0)
}

}  // namespace

int main() {
    run_unit_tests();
    run_integration_tests();

    std::printf(g_fail == 0 ? "\nALL CHARACTER-CLI TESTS PASSED\n"
                            : "\n%d CHARACTER-CLI TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
