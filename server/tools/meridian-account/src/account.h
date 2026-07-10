// SPDX-License-Identifier: Apache-2.0
//
// meridian-account — account-creation code path (ACC-03, M0).
//
// Provenance: designed from the server SAD §2.1 (authd: "SRP6a-style verifier
// scheme; passwords never stored/transmitted in plaintext"; the meridian-account
// CLI is the M0 account-creation path) and §4.1 (auth DB `account` table field
// list). Composes two merged libs — meridian-srp (make_verifier) and
// meridian-db (Connection, prepared statements). Clean-room, original code:
// no GPL source consulted (CONTRIBUTING.md).
//
// This header exposes the account-creation code path as a library function so
// the integration test can drive the exact path the CLI uses. The server never
// sees or stores a plaintext password: registration derives {salt, verifier}
// (SAD §2.1) and INSERTs them through a prepared statement (never concatenated).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "meridian/db/connection.h"
#include "meridian/srp/srp.h"

namespace meridian::account {

// Parameters for one account-creation request.
struct CreateRequest {
    std::string username;
    std::string password;
    std::uint8_t gm_level = 0;
    // Optional account email (account.email is NULLable, §4.1). Empty => NULL.
    std::string email;
};

// Result of a successful create: the server-minted account id and the derived
// SRP credential that was stored (returned so callers/tests can verify it).
struct CreateResult {
    std::uint64_t account_id = 0;
    srp::Verifier credential;  // {salt, verifier} as stored in the auth DB
};

// Thrown when the username already exists (auth DB uq_account_username, §4.1).
// The CLI maps this to a clear message + a distinct non-zero exit code.
class DuplicateUsername : public std::runtime_error {
public:
    explicit DuplicateUsername(const std::string& username)
        : std::runtime_error("username already exists: " + username) {}
};

// Derive {salt, verifier} via meridian::srp::make_verifier using the production
// defaults (2048-bit group + SHA-256), then INSERT the account into the auth DB
// `account` table through a prepared statement. salt/verifier bind as blobs
// (VARBINARY columns srp_salt/srp_verifier). Returns the new account id and the
// stored credential.
//
// Throws DuplicateUsername on a UNIQUE-key violation, meridian::db::DbError on
// any other DB failure, std::invalid_argument on empty username.
CreateResult create_account(db::Connection& conn, const CreateRequest& req,
                            const srp::Parameters& params = {});

// Set an existing account's GM permission level (auth DB account.gm_level, D-16
// ladder: 0 player < 1 helper < 2 GM < 3 admin). This is how an operator grants
// (or revokes) GM rights on a live account, and how the GM-command tests (OPS-02a
// #417) promote a seeded account to a GM level. Parameterized UPDATE keyed by the
// unique username (never concatenated). Returns true when a row was updated,
// false when no account has that username. Throws meridian::db::DbError on a DB
// failure, std::invalid_argument on an empty username.
bool set_gm_level(db::Connection& conn, const std::string& username,
                  std::uint8_t gm_level);

}  // namespace meridian::account
