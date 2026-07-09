// SPDX-License-Identifier: Apache-2.0
//
// meridian-account — the M0 account-creation CLI (ACC-03 basic).
//
// Provenance: designed from the server SAD §2.1 (authd account-creation path,
// SRP6a verifier scheme — passwords never stored/transmitted in plaintext) and
// §4.1 (auth DB `account` table). Composes meridian-srp (make_verifier, 2048-bit
// group + SHA-256 production defaults) and meridian-db (Connection, prepared
// statements). Clean-room, original code; no GPL source consulted.
//
// Usage:
//   meridian-account create --username U [--password P] [--gm-level N] [--email E]
//
// If --password is omitted the password is read from stdin. When stdin is a TTY
// the input is not echoed (and confirmed); when stdin is a pipe the first line
// is read as the password (for test automation).
//
// DB connection comes from --db-* flags or MERIDIAN_DB_* env (same var names as
// meridian-db's integration test): MERIDIAN_DB_HOST/PORT/USER/PASS/NAME/SOCKET.
//
// Exit codes:
//   0  success
//   1  bad usage / missing args
//   2  DB connection or query error
//   3  duplicate username
//   4  password input error

#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

#include "account.h"
#include "meridian/db/connection.h"
#include "meridian/srp/srp.h"

namespace {

constexpr int kOk = 0;
constexpr int kUsage = 1;
constexpr int kDbError = 2;
constexpr int kDuplicate = 3;
constexpr int kPasswordError = 4;
constexpr int kNotFound = 5;  // set-gm-level: no account with that username

const char* env(const char* k) { return std::getenv(k); }

void usage(std::FILE* out) {
    std::fprintf(out,
        "meridian-account — Project Meridian account CLI (ACC-03 / OPS-02a)\n"
        "\n"
        "Usage:\n"
        "  meridian-account create --username U [--password P] [--gm-level N] [--email E]\n"
        "                          [--db-host H] [--db-port P] [--db-user U]\n"
        "                          [--db-pass P] [--db-name N] [--db-socket S]\n"
        "  meridian-account set-gm-level --username U --gm-level N\n"
        "                          [--db-host H] [--db-port P] [--db-user U]\n"
        "                          [--db-pass P] [--db-name N] [--db-socket S]\n"
        "\n"
        "  create      : register a new account (SRP6a verifier).\n"
        "  set-gm-level: change an existing account's GM level (D-16 ladder:\n"
        "                0 player < 1 helper < 2 GM < 3 admin).\n"
        "\n"
        "  If --password is omitted it is read from stdin (no echo on a TTY).\n"
        "\n"
        "DB connection defaults come from the environment (same names as\n"
        "meridian-db's test): MERIDIAN_DB_HOST, MERIDIAN_DB_PORT, MERIDIAN_DB_USER,\n"
        "MERIDIAN_DB_PASS, MERIDIAN_DB_NAME, MERIDIAN_DB_SOCKET.\n");
}

// Read a password from stdin. On a TTY: prompt, disable echo, read a line, and
// require confirmation. On a pipe/file (test automation): read the first line
// verbatim, no prompt, no confirmation.
std::optional<std::string> read_password_from_stdin() {
    if (::isatty(STDIN_FILENO)) {
        termios old{};
        if (::tcgetattr(STDIN_FILENO, &old) != 0) return std::nullopt;
        termios noecho = old;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);

        auto read_line = [&](const char* prompt) -> std::optional<std::string> {
            std::fprintf(stderr, "%s", prompt);
            std::fflush(stderr);
            if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho) != 0) return std::nullopt;
            std::string line;
            bool ok = static_cast<bool>(std::getline(std::cin, line));
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
            std::fprintf(stderr, "\n");
            if (!ok) return std::nullopt;
            return line;
        };

        auto p1 = read_line("Password: ");
        if (!p1) return std::nullopt;
        auto p2 = read_line("Confirm password: ");
        if (!p2) return std::nullopt;
        if (*p1 != *p2) {
            std::fprintf(stderr, "error: passwords do not match\n");
            return std::nullopt;
        }
        return p1;
    }

    // Non-TTY: read the first line (strip a trailing newline) as the password.
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
}

// Populate ConnectParams from MERIDIAN_DB_* env, then overlay any --db-* flags.
struct DbFlags {
    std::optional<std::string> host, user, pass, name, socket;
    std::optional<unsigned> port;
};

meridian::db::ConnectParams build_conn_params(const DbFlags& f) {
    meridian::db::ConnectParams p;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) p.unix_socket = s;
    if (const char* h = env("MERIDIAN_DB_HOST")) p.host = h;
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    if (const char* n = env("MERIDIAN_DB_NAME")) p.database = n;

    if (f.socket) p.unix_socket = *f.socket;
    if (f.host) p.host = *f.host;
    if (f.port) p.port = *f.port;
    if (f.user) p.user = *f.user;
    if (f.pass) p.password = *f.pass;
    if (f.name) p.database = *f.name;
    return p;
}

// Minimal flag parser: --flag value. Returns the value or nullptr; advances i.
const char* take_value(int argc, char** argv, int& i) {
    if (i + 1 >= argc) return nullptr;
    return argv[++i];
}

int cmd_create(int argc, char** argv) {
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::optional<std::string> email;
    long gm_level = 0;
    DbFlags db;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](std::optional<std::string>& dst) -> bool {
            const char* v = take_value(argc, argv, i);
            if (!v) { std::fprintf(stderr, "error: %s requires a value\n", a.c_str()); return false; }
            dst = v; return true;
        };
        if (a == "--username") { if (!need(username)) return kUsage; }
        else if (a == "--password") { if (!need(password)) return kUsage; }
        else if (a == "--email") { if (!need(email)) return kUsage; }
        else if (a == "--gm-level") {
            const char* v = take_value(argc, argv, i);
            if (!v) { std::fprintf(stderr, "error: --gm-level requires a value\n"); return kUsage; }
            char* end = nullptr;
            gm_level = std::strtol(v, &end, 10);
            if (*end != '\0' || gm_level < 0 || gm_level > 255) {
                std::fprintf(stderr, "error: --gm-level must be 0..255\n");
                return kUsage;
            }
        }
        else if (a == "--db-host") { if (!need(db.host)) return kUsage; }
        else if (a == "--db-port") {
            const char* v = take_value(argc, argv, i);
            if (!v) { std::fprintf(stderr, "error: --db-port requires a value\n"); return kUsage; }
            db.port = static_cast<unsigned>(std::atoi(v));
        }
        else if (a == "--db-user") { if (!need(db.user)) return kUsage; }
        else if (a == "--db-pass") { if (!need(db.pass)) return kUsage; }
        else if (a == "--db-name") { if (!need(db.name)) return kUsage; }
        else if (a == "--db-socket") { if (!need(db.socket)) return kUsage; }
        else { std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str()); return kUsage; }
    }

    if (!username || username->empty()) {
        std::fprintf(stderr, "error: --username is required\n");
        return kUsage;
    }

    if (!password) {
        auto p = read_password_from_stdin();
        if (!p) { std::fprintf(stderr, "error: failed to read password\n"); return kPasswordError; }
        password = *p;
    }

    meridian::db::ConnectParams cp = build_conn_params(db);
    if (cp.user.empty()) {
        std::fprintf(stderr,
            "error: no DB user configured (set --db-user or MERIDIAN_DB_USER)\n");
        return kUsage;
    }

    try {
        meridian::db::Connection conn(cp);
        meridian::account::CreateRequest req;
        req.username = *username;
        req.password = *password;
        req.gm_level = static_cast<std::uint8_t>(gm_level);
        if (email) req.email = *email;

        // Production defaults: 2048-bit group + SHA-256 (srp::Parameters{}).
        auto res = meridian::account::create_account(conn, req);
        std::printf("created account '%s' (id=%llu, gm_level=%ld, verifier=%zu bytes)\n",
                    username->c_str(),
                    static_cast<unsigned long long>(res.account_id),
                    gm_level, res.credential.verifier.size());
        return kOk;
    } catch (const meridian::account::DuplicateUsername& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kDuplicate;
    } catch (const meridian::db::DbError& e) {
        std::fprintf(stderr, "error: database error (%u): %s\n", e.code(), e.what());
        return kDbError;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kDbError;
    }
}

// set-gm-level --username U --gm-level N : grant/revoke GM rights on an existing
// account (OPS-02a #417). Keyed by the unique username; N is the D-16 raw level.
int cmd_set_gm_level(int argc, char** argv) {
    std::optional<std::string> username;
    std::optional<long> gm_level;
    DbFlags db;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](std::optional<std::string>& dst) -> bool {
            const char* v = take_value(argc, argv, i);
            if (!v) { std::fprintf(stderr, "error: %s requires a value\n", a.c_str()); return false; }
            dst = v; return true;
        };
        if (a == "--username") { if (!need(username)) return kUsage; }
        else if (a == "--gm-level") {
            const char* v = take_value(argc, argv, i);
            if (!v) { std::fprintf(stderr, "error: --gm-level requires a value\n"); return kUsage; }
            char* end = nullptr;
            long lvl = std::strtol(v, &end, 10);
            if (*end != '\0' || lvl < 0 || lvl > 255) {
                std::fprintf(stderr, "error: --gm-level must be 0..255\n");
                return kUsage;
            }
            gm_level = lvl;
        }
        else if (a == "--db-host") { if (!need(db.host)) return kUsage; }
        else if (a == "--db-port") {
            const char* v = take_value(argc, argv, i);
            if (!v) { std::fprintf(stderr, "error: --db-port requires a value\n"); return kUsage; }
            db.port = static_cast<unsigned>(std::atoi(v));
        }
        else if (a == "--db-user") { if (!need(db.user)) return kUsage; }
        else if (a == "--db-pass") { if (!need(db.pass)) return kUsage; }
        else if (a == "--db-name") { if (!need(db.name)) return kUsage; }
        else if (a == "--db-socket") { if (!need(db.socket)) return kUsage; }
        else { std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str()); return kUsage; }
    }

    if (!username || username->empty()) {
        std::fprintf(stderr, "error: --username is required\n");
        return kUsage;
    }
    if (!gm_level) {
        std::fprintf(stderr, "error: --gm-level is required\n");
        return kUsage;
    }

    meridian::db::ConnectParams cp = build_conn_params(db);
    if (cp.user.empty()) {
        std::fprintf(stderr,
            "error: no DB user configured (set --db-user or MERIDIAN_DB_USER)\n");
        return kUsage;
    }

    try {
        meridian::db::Connection conn(cp);
        bool updated = meridian::account::set_gm_level(
            conn, *username, static_cast<std::uint8_t>(*gm_level));
        if (!updated) {
            std::fprintf(stderr, "error: no account named '%s'\n", username->c_str());
            return kNotFound;
        }
        std::printf("set account '%s' gm_level=%ld\n", username->c_str(), *gm_level);
        return kOk;
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
    if (sub == "set-gm-level") return cmd_set_gm_level(argc - 2, argv + 2);

    std::fprintf(stderr, "error: unknown subcommand '%s'\n\n", sub.c_str());
    usage(stderr);
    return kUsage;
}
