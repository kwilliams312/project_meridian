// SPDX-License-Identifier: Apache-2.0
//
// meridian-character — character-creation CLI for test provisioning / the
// concurrency-harness load driver (mirrors meridian-account, #80).
//
// Usage:
//   meridian-character create --username U [--name NAME] [--race R] [--class C]
//
// Resolves the account_id for --username from the auth DB, then creates a
// character for it through the existing characters CRUD
// (meridian::characters::create_character, server/characters/src/characters.h).
//
// DB connection comes from --db-* flags or MERIDIAN_DB_* env (same var names
// as meridian-account / meridian-db's integration test):
// MERIDIAN_DB_HOST/PORT/USER/PASS/NAME/SOCKET. Unlike meridian-account, the
// connection's default database is the CHARACTERS db ("meridian_characters")
// when unset by env/flag — create_character() queries the unqualified
// `character` table. The account_id lookup is a fully-qualified cross-schema
// SELECT against meridian_auth.account on the SAME connection (the
// `meridian` DB user has grants on both schemas; server SAD §4.4 soft-ref
// rule — no FK, numeric id only).
//
// Exit codes:
//   0  success
//   1  bad usage / missing args
//   2  invalid request (bad name / race / class — see characters.h)
//   3  already exists / at cap (duplicate name, or account already has a
//      character) — treated as a benign "skip" for idempotent provisioning
//   4  unknown account (no auth-DB row for --username)
//   5  database error

#include <cstdio>
#include <string>

#include "character.h"
#include "meridian/db/connection.h"

namespace {

constexpr int kOk = 0;
constexpr int kUsage = 1;
constexpr int kInvalidRequest = 2;
constexpr int kAlreadyExists = 3;
constexpr int kUnknownAccount = 4;
constexpr int kDbError = 5;

void usage(std::FILE* out) {
    std::fprintf(out,
        "meridian-character — Project Meridian character-creation CLI\n"
        "\n"
        "Usage:\n"
        "  meridian-character create --username U [--name NAME] [--race R] [--class C]\n"
        "                            [--db-host H] [--db-port P] [--db-user U]\n"
        "                            [--db-pass P] [--db-name N] [--db-socket S]\n"
        "\n"
        "  --name defaults to --username. --race defaults to 1, --class to 1\n"
        "  (roster.h M0 defaults: Ardent / Vanguard).\n"
        "\n"
        "DB connection defaults come from the environment (same names as\n"
        "meridian-account / meridian-db's test): MERIDIAN_DB_HOST, MERIDIAN_DB_PORT,\n"
        "MERIDIAN_DB_USER, MERIDIAN_DB_PASS, MERIDIAN_DB_NAME, MERIDIAN_DB_SOCKET.\n"
        "The connection's default database is the characters DB\n"
        "(meridian_characters) unless overridden.\n");
}

int cmd_create(int argc, char** argv) {
    using namespace meridian::character_cli;

    ParseResult parsed = parse_create_args(argc, argv);
    if (parsed.outcome != ParseOutcome::kOk) {
        std::fprintf(stderr, "error: %s\n", parsed.error.c_str());
        return kUsage;
    }

    meridian::db::ConnectParams cp = build_conn_params(parsed.db);
    if (cp.user.empty()) {
        std::fprintf(stderr,
            "error: no DB user configured (set --db-user or MERIDIAN_DB_USER)\n");
        return kUsage;
    }

    try {
        meridian::db::Connection conn(cp);
        CreateForAccountResult res = create_character_for_account(conn, parsed.args);
        std::printf(
            "created character '%s' (id=%llu, account=%s) for account_id=%llu\n",
            effective_name(parsed.args).c_str(),
            static_cast<unsigned long long>(res.character.character_id),
            parsed.args.username.c_str(),
            static_cast<unsigned long long>(res.account_id));
        return kOk;
    } catch (const UnknownAccount& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kUnknownAccount;
    } catch (const meridian::characters::DuplicateName& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kAlreadyExists;
    } catch (const meridian::characters::CharacterLimitReached& e) {
        std::fprintf(stderr, "error: %s (skipping — already provisioned)\n", e.what());
        return kAlreadyExists;
    } catch (const meridian::characters::InvalidName& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kInvalidRequest;
    } catch (const meridian::characters::InvalidRace& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kInvalidRequest;
    } catch (const meridian::characters::InvalidClass& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kInvalidRequest;
    } catch (const meridian::db::DbError& e) {
        std::fprintf(stderr, "error: database error (%u): %s\n", e.code(), e.what());
        return kDbError;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kDbError;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(stderr); return kUsage; }

    std::string sub = argv[1];
    if (sub == "-h" || sub == "--help" || sub == "help") { usage(stdout); return kOk; }
    if (sub == "create") return cmd_create(argc - 2, argv + 2);

    std::fprintf(stderr, "error: unknown subcommand '%s'\n\n", sub.c_str());
    usage(stderr);
    return kUsage;
}
