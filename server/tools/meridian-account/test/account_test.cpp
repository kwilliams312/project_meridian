// SPDX-License-Identifier: Apache-2.0
//
// meridian-account integration test (ACC-03, issue #80).
//
// Needs a live MariaDB; reads connection params from MERIDIAN_DB_* env (same
// var names as meridian-db's test) and SKIPS (exit 0) when none are set, so it
// is inert in the plain server build's ctest and runs for real only in the
// account-CLI CI job (or locally with env set).
//
// What it proves end-to-end:
//   1. create_account() (the CLI's account-creation code path) derives a
//      {salt, verifier} via meridian::srp::make_verifier (production defaults:
//      2048-bit group + SHA-256) and INSERTs it into the auth DB `account`
//      table through a prepared statement.
//   2. Reading {srp_salt, srp_verifier} back from the DB and running a FULL
//      SRP-6a exchange (ServerSession + testing::client_side) authenticates the
//      CORRECT password (M1 verifies, M2 matches) and REJECTS a wrong password.
//   3. A duplicate username is refused (DuplicateUsername) — the UNIQUE key.
//
// Clean-room, original code; no GPL source consulted (CONTRIBUTING.md).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "account.h"
#include "meridian/db/connection.h"
#include "meridian/srp/srp.h"

using namespace meridian;

namespace {
int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Convert a DB cell (raw bytes carried in a std::string) to srp::Bytes.
srp::Bytes cell_to_bytes(const std::string& s) {
    return srp::Bytes(s.begin(), s.end());
}

// Minimal DDL for the account table (mirrors 0001_init_auth.up.sql, no FKs so
// it stands alone in a fresh schema). The test creates it if absent, then
// cleans up its own test rows — it never drops a table it did not create.
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
}  // namespace

int main() {
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

    // Unique username per run so repeated local runs don't collide.
    const std::string username = "acc_test_" + std::to_string(std::rand());
    const std::string password = "correct horse battery staple";
    const std::string wrong_password = "Tr0ub4dour&3";

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kAccountDdl);
        // Clean any stray row from a prior aborted run.
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});

        // ---- 1. Create the account through the CLI's code path -------------
        account::CreateRequest req;
        req.username = username;
        req.password = password;
        req.gm_level = 3;
        req.email = "acc_test@example.invalid";
        account::CreateResult created = account::create_account(db, req);
        check("account created (id assigned)", created.account_id > 0);
        check("verifier is 256 bytes (2048-bit group)", created.credential.verifier.size() == 256);

        // ---- 2. Read {salt, verifier} back from the DB ---------------------
        db::Result row = db.execute(
            "SELECT id, srp_salt, srp_verifier, gm_level, email FROM account WHERE username = ?",
            {db::Param{username}});
        check("row read back", row.rows.size() == 1);
        if (row.rows.size() != 1) { std::printf("\n1 ACCOUNT TEST FAILED (no row)\n"); return 1; }

        const db::Row& r = row.rows[0];
        check("gm_level stored", r[3].has_value() && *r[3] == "3");
        check("email stored", r[4].has_value() && *r[4] == "acc_test@example.invalid");
        srp::Bytes salt = cell_to_bytes(*r[1]);
        srp::Bytes verifier = cell_to_bytes(*r[2]);
        check("salt is 32 bytes", salt.size() == 32);
        check("verifier is 256 bytes from DB", verifier.size() == 256);
        check("stored salt matches derived", salt == created.credential.salt);
        check("stored verifier matches derived", verifier == created.credential.verifier);

        // ---- 3. Full SRP-6a exchange with the CORRECT password -------------
        // Production defaults: 2048-bit group + SHA-256. This is exactly the
        // authd login path (SAD §5.1) against the credential the CLI stored.
        srp::Parameters params;  // {Rfc5054_2048, Sha256}
        {
            srp::ServerSession server(username, salt, verifier, params);
            // Fixed client ephemeral a (deterministic; production picks random).
            srp::Bytes fixed_a = {
                0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0xA7, 0xB8,
                0xC9, 0xD0, 0xE1, 0xF2, 0xA3, 0xB4, 0xC5, 0xD6,
                0xE7, 0xF8, 0xA9, 0xB0, 0xC1, 0xD2, 0xE3, 0xF4,
                0xA5, 0xB6, 0xC7, 0xD8, 0xE9, 0xF0, 0xA1, 0xB2};
            srp::testing::ClientProof cp =
                srp::testing::client_side(username, password, salt, fixed_a, server.B(), params);
            std::optional<srp::Bytes> m2 = server.verify(cp.A, cp.M1);
            check("SRP: correct password authenticates (M1 verifies)", m2.has_value());
            check("SRP: server M2 matches client's expected M2",
                  m2.has_value() && *m2 == cp.expected_M2);
        }

        // ---- 4. Full SRP-6a exchange with a WRONG password -> rejected -----
        {
            srp::ServerSession server(username, salt, verifier, params);
            srp::Bytes fixed_a = {
                0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
                0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
            srp::testing::ClientProof bad =
                srp::testing::client_side(username, wrong_password, salt, fixed_a, server.B(), params);
            std::optional<srp::Bytes> m2 = server.verify(bad.A, bad.M1);
            check("SRP: wrong password is REJECTED (no M2)", !m2.has_value());
        }

        // ---- 4b. set_gm_level changes an existing account's GM level (#417) --
        // The account was created at gm_level=3; demote it to 1 (helper), then
        // read it back. A non-existent username updates nothing (returns false).
        {
            bool updated = account::set_gm_level(db, username, 1);
            check("set_gm_level: existing account updated", updated);
            db::Result lv = db.execute(
                "SELECT gm_level FROM account WHERE username = ?", {db::Param{username}});
            check("set_gm_level: new level persisted",
                  lv.rows.size() == 1 && lv.rows[0][0].has_value() && *lv.rows[0][0] == "1");
            bool missing = account::set_gm_level(db, "no_such_user_" + username, 2);
            check("set_gm_level: unknown username updates nothing", !missing);
        }

        // ---- 5. Duplicate username is refused ------------------------------
        bool threw_dup = false;
        try {
            account::create_account(db, req);
        } catch (const account::DuplicateUsername&) {
            threw_dup = true;
        }
        check("duplicate username refused (UNIQUE key)", threw_dup);

        // Clean up this run's row.
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL ACCOUNT TESTS PASSED\n"
                            : "\n%d ACCOUNT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
