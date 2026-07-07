// SPDX-License-Identifier: Apache-2.0
//
// worldd grant-lifecycle test (issue #81 — authd test suite hardening).
//
// CLEAN-ROOM: written from the server SAD (§3.1 single-use grant, §4.1
// session_grant DDL, §5.3 realm binding) and the meridian-db public API only.
// No GPL source consulted (CONTRIBUTING.md).
//
// This test drives validate_and_consume_grant() DIRECTLY (not through the wire),
// so it can assert the GrantReject CLASSIFICATION — the reason a grant was
// rejected. The end-to-end session test (worldd-session-test, #84) proves the
// wire behaviour, but every grant failure collapses to one opaque GRANT_INVALID
// Disconnect on the wire (no oracle to the client), so it CANNOT observe WHY a
// grant was rejected. The classification (unknown / expired / already-consumed /
// wrong-realm) is the branch this test pins down, over a real MariaDB:
//
//   CREATE  -> a valid grant validates ONCE, returning {account, realm, key}.
//   REPLAY  -> the SAME grant a second time -> kAlreadyConsumed, no re-consume.
//   EXPIRED -> a past-expiry grant -> kExpired, and it is NOT consumed.
//   WRONG-REALM -> a valid grant bound to another realm -> kWrongRealm; it IS
//                  consumed (spent), so single-use holds even on the reject.
//   UNKNOWN -> a grant_id with no row -> kUnknown.
//
// Needs a live MariaDB with the auth schema loaded (0001_init_auth.up.sql). Reads
// MERIDIAN_DB_* env (same vars as the db/account/authd/worldd tests) and SKIPS
// (exit 0) when none are set, so it is inert in the plain server build's ctest
// and runs for real only in CI (or locally with env set).

#include "world_session.h"

#include "meridian/db/connection.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace meridian;
namespace mw = meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

std::uint64_t cell_u64(const db::Cell& c) {
    return c.has_value() ? std::strtoull(c->c_str(), nullptr, 10) : 0;
}

db::Param blob(const Bytes& b) {
    return db::Param{db::Bytes_t(b.begin(), b.end())};
}

// Seed a session_grant row EXACTLY as authd writes it (SAD §4.1): decimal-string
// binding for the BIGINT UNSIGNED grant_id/account_id. `expires_sql` is the
// DATETIME expression (a future or a past time).
void seed_grant(db::Connection& db, std::uint64_t grant_id, std::uint64_t account_id,
                std::uint32_t realm_id, const Bytes& session_key,
                std::uint32_t client_build, const std::string& expires_sql) {
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, " + expires_sql + ")",
        {db::Param{std::to_string(grant_id)},
         db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)},
         blob(session_key),
         db::Param{static_cast<std::int64_t>(client_build)}});
}

bool grant_consumed(db::Connection& db, std::uint64_t grant_id) {
    db::Result r = db.execute(
        "SELECT consumed_at FROM session_grant WHERE grant_id = ?",
        {db::Param{std::to_string(grant_id)}});
    return r.rows.size() == 1 && r.rows[0][0].has_value();
}

}  // namespace

int main() {
    std::printf("worldd grant-lifecycle test (validate_and_consume_grant, #81)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured (set "
                    "MERIDIAN_DB_SOCKET or MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        return 0;
    }

    const std::uint32_t client_build = 1000;
    std::uint64_t account_id = 0;
    std::uint32_t realm_id = 0, realm2_id = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");

        // --- Seed an account + two realms (session_grant FKs both). -----------
        const std::string username = "grantlc_it_" + std::to_string(std::rand());
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
            {db::Param{username}, blob(Bytes(32, 0x11)), blob(Bytes(32, 0x22))});
        db::Result ar = db.execute("SELECT id FROM account WHERE username = ?",
                                   {db::Param{username}});
        account_id = cell_u64(ar.rows.at(0)[0]);
        check("account seeded", account_id > 0);

        auto seed_realm = [&](const std::string& name) -> std::uint32_t {
            db.execute(
                "INSERT INTO realm (name, address, port, build_min, build_max) "
                "VALUES (?, '127.0.0.1', 7200, 0, 100000)",
                {db::Param{name}});
            db::Result rr = db.execute("SELECT id FROM realm WHERE name = ?",
                                       {db::Param{name}});
            return static_cast<std::uint32_t>(cell_u64(rr.rows.at(0)[0]));
        };
        realm_id = seed_realm("GLC Realm " + std::to_string(std::rand()));
        realm2_id = seed_realm("GLC Realm2 " + std::to_string(std::rand()));
        check("two realms seeded", realm_id > 0 && realm2_id > 0 && realm_id != realm2_id);

        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        const std::uint64_t g_ok = rand_u64();
        const std::uint64_t g_expired = rand_u64();
        const std::uint64_t g_wrong_realm = rand_u64();
        const std::uint64_t g_unknown = 0xDEADBEEFFEEDFACEULL;  // never inserted
        const Bytes session_key(32, 0xC7);

        seed_grant(db, g_ok, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        seed_grant(db, g_expired, account_id, realm_id, session_key, client_build,
                   "DATE_SUB(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        seed_grant(db, g_wrong_realm, account_id, realm2_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        check("valid + expired + wrong-realm grants seeded", true);

        // ===== CREATE: a valid grant validates ONCE, returns the bound data ===
        {
            mw::GrantReject reject = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g =
                mw::validate_and_consume_grant(db, g_ok, realm_id, reject);
            check("CREATE: valid grant accepted", g.has_value());
            if (g) {
                check("CREATE: account_id matches", g->account_id == account_id);
                check("CREATE: realm_id matches", g->realm_id == realm_id);
                check("CREATE: session_key is 32 bytes", g->session_key.size() == 32);
                check("CREATE: session_key round-trips", g->session_key == session_key);
            }
            check("CREATE: grant now marked consumed", grant_consumed(db, g_ok));
        }

        // ===== REPLAY: the SAME grant again -> kAlreadyConsumed, no re-consume =
        {
            mw::GrantReject reject = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g =
                mw::validate_and_consume_grant(db, g_ok, realm_id, reject);
            check("REPLAY: second use rejected", !g.has_value());
            check("REPLAY: reason is kAlreadyConsumed",
                  reject == mw::GrantReject::kAlreadyConsumed);
        }

        // ===== EXPIRED: past-expiry grant -> kExpired, NOT consumed ============
        {
            mw::GrantReject reject = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g =
                mw::validate_and_consume_grant(db, g_expired, realm_id, reject);
            check("EXPIRED: rejected", !g.has_value());
            check("EXPIRED: reason is kExpired", reject == mw::GrantReject::kExpired);
            check("EXPIRED: grant NOT consumed", !grant_consumed(db, g_expired));
        }

        // ===== WRONG-REALM: valid grant for realm2, served by realm_id ========
        // Rejected as kWrongRealm, but the atomic consume already spent it — so
        // single-use holds and it cannot be replayed against realm2 either.
        {
            mw::GrantReject reject = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g =
                mw::validate_and_consume_grant(db, g_wrong_realm, realm_id, reject);
            check("WRONG-REALM: rejected", !g.has_value());
            check("WRONG-REALM: reason is kWrongRealm",
                  reject == mw::GrantReject::kWrongRealm);
            check("WRONG-REALM: grant IS consumed (single-use preserved on reject)",
                  grant_consumed(db, g_wrong_realm));

            // And a retry against the CORRECT realm now fails too — it is spent.
            mw::GrantReject reject2 = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g2 =
                mw::validate_and_consume_grant(db, g_wrong_realm, realm2_id, reject2);
            check("WRONG-REALM: spent grant unusable on its own realm too",
                  !g2.has_value() && reject2 == mw::GrantReject::kAlreadyConsumed);
        }

        // ===== UNKNOWN: a grant_id with no row -> kUnknown ====================
        {
            mw::GrantReject reject = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g =
                mw::validate_and_consume_grant(db, g_unknown, realm_id, reject);
            check("UNKNOWN: rejected", !g.has_value());
            check("UNKNOWN: reason is kUnknown", reject == mw::GrantReject::kUnknown);
        }

        // ===== expected_realm_id == 0 disables the realm check ================
        // (SAD: a worldd with realm_id unset accepts any realm's grant.) Seed a
        // fresh valid grant for realm2 and consume it with expected_realm_id 0.
        {
            const std::uint64_t g_anyrealm = rand_u64();
            seed_grant(db, g_anyrealm, account_id, realm2_id, session_key, client_build,
                       "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
            mw::GrantReject reject = mw::GrantReject::kDbError;
            std::optional<mw::GrantConsumed> g =
                mw::validate_and_consume_grant(db, g_anyrealm, /*expected_realm_id=*/0, reject);
            check("ANY-REALM: expected_realm_id 0 accepts any realm's grant",
                  g.has_value() && g->realm_id == realm2_id);
        }

        // --- Cleanup: grants (CASCADE via account) + both realms + account. ---
        db.execute("DELETE FROM session_grant WHERE account_id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
        db.execute("DELETE FROM realm WHERE id IN (?, ?)",
                   {db::Param{static_cast<std::int64_t>(realm_id)},
                    db::Param{static_cast<std::int64_t>(realm2_id)}});
        db.execute("DELETE FROM account WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL GRANT-LIFECYCLE TESTS PASSED\n"
                            : "\n%d GRANT-LIFECYCLE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
