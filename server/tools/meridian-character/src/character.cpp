// SPDX-License-Identifier: Apache-2.0
//
// meridian-character — character-creation CLI code path.
// See character.h for provenance / the cross-DB soft-ref note.

#include "character.h"

#include <cstdlib>
#include <string>

namespace meridian::character_cli {

namespace {

const char* env(const char* k) { return std::getenv(k); }

// Minimal flag parser: --flag value. Returns the value or nullptr; advances i.
// Same convention as meridian-account's take_value.
const char* take_value(int argc, char** argv, int& i) {
    if (i + 1 >= argc) return nullptr;
    return argv[++i];
}

// Parse a u8 in [0, 255] from `v`. Returns false (and leaves *out untouched)
// on a malformed or out-of-range value.
bool parse_u8(const char* v, std::uint8_t* out) {
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (v[0] == '\0' || *end != '\0' || n < 0 || n > 255) return false;
    *out = static_cast<std::uint8_t>(n);
    return true;
}

// Parse a BIGINT UNSIGNED result cell (carried as text by meridian-db) to a
// uint64. Mirrors characters.cpp's parse_u64 (account.id has the same
// BIGINT UNSIGNED / decimal-string round-trip shape as character.id).
std::uint64_t parse_u64(const db::Cell& c) {
    if (!c.has_value() || c->empty()) return 0;
    return std::stoull(*c);
}

}  // namespace

ParseResult parse_create_args(int argc, char** argv) {
    ParseResult result;
    CreateArgs args;
    DbFlags db;
    std::optional<std::string> username;
    std::optional<std::string> name;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_str = [&](std::optional<std::string>& dst) -> bool {
            const char* v = take_value(argc, argv, i);
            if (!v) { result.error = a + " requires a value"; return false; }
            dst = v;
            return true;
        };
        auto need_u8 = [&](std::uint8_t& dst) -> bool {
            const char* v = take_value(argc, argv, i);
            if (!v) { result.error = a + " requires a value"; return false; }
            if (!parse_u8(v, &dst)) { result.error = a + " must be 0..255"; return false; }
            return true;
        };

        if (a == "--username") { if (!need_str(username)) return result; }
        else if (a == "--name") { if (!need_str(name)) return result; }
        else if (a == "--race") { if (!need_u8(args.race)) return result; }
        else if (a == "--class") { if (!need_u8(args.char_class)) return result; }
        else if (a == "--db-host") { if (!need_str(db.host)) return result; }
        else if (a == "--db-port") {
            const char* v = take_value(argc, argv, i);
            if (!v) { result.error = "--db-port requires a value"; return result; }
            db.port = static_cast<unsigned>(std::atoi(v));
        }
        else if (a == "--db-user") { if (!need_str(db.user)) return result; }
        else if (a == "--db-pass") { if (!need_str(db.pass)) return result; }
        else if (a == "--db-name") { if (!need_str(db.name)) return result; }
        else if (a == "--db-socket") { if (!need_str(db.socket)) return result; }
        else { result.error = "unknown option '" + a + "'"; return result; }
    }

    if (!username || username->empty()) {
        result.error = "--username is required";
        return result;
    }

    args.username = *username;
    args.name = name;
    result.outcome = ParseOutcome::kOk;
    result.args = args;
    result.db = db;
    return result;
}

std::string effective_name(const CreateArgs& args) {
    return args.name.value_or(args.username);
}

db::ConnectParams build_conn_params(const DbFlags& f) {
    db::ConnectParams p;
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

    // create_character() issues unqualified `character` table queries, so the
    // connection's default DB must be the characters schema unless the caller
    // explicitly overrode it (env or --db-name).
    if (p.database.empty()) p.database = "meridian_characters";
    return p;
}

std::uint64_t resolve_account_id(db::Connection& conn, const std::string& username) {
    db::Result r = conn.execute(
        "SELECT id FROM meridian_auth.account WHERE username = ?",
        {db::Param{username}});
    if (r.rows.empty()) throw UnknownAccount(username);
    return parse_u64(r.rows[0][0]);
}

CreateForAccountResult create_character_for_account(db::Connection& conn,
                                                     const CreateArgs& args) {
    CreateForAccountResult result;
    result.account_id = resolve_account_id(conn, args.username);

    characters::CreateRequest req;
    req.account_id = result.account_id;
    req.name = effective_name(args);
    req.race = args.race;
    req.char_class = args.char_class;

    result.character = characters::create_character(conn, req);
    return result;
}

}  // namespace meridian::character_cli
