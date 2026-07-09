// SPDX-License-Identifier: Apache-2.0
//
// meridian-account — account-creation code path (ACC-03, M0).
// See account.h for provenance / clean-room statement.

#include "account.h"

#include <stdexcept>

namespace meridian::account {

namespace {

// MariaDB / MySQL error number for a duplicate entry on a UNIQUE key.
// (ER_DUP_ENTRY, see MariaDB error reference — not GPL source.)
constexpr unsigned kErDupEntry = 1062;

// Convert srp Bytes (vector<uint8_t>) to the db blob param type (also
// vector<uint8_t>). Kept explicit so the two libs stay decoupled.
db::Bytes_t to_blob(const srp::Bytes& b) {
    return db::Bytes_t(b.begin(), b.end());
}

}  // namespace

CreateResult create_account(db::Connection& conn, const CreateRequest& req,
                            const srp::Parameters& params) {
    if (req.username.empty()) {
        throw std::invalid_argument("username must not be empty");
    }

    // Derive {salt, verifier}. The plaintext password stays in this stack frame
    // and is never sent to the DB — only the verifier is (SAD §2.1).
    srp::Verifier cred = srp::make_verifier(req.username, req.password, params);

    // Parameterized INSERT — every value binds through a placeholder; user input
    // is NEVER concatenated into SQL (meridian-db safety rule; SAD backend
    // standard). salt/verifier bind as blobs into the VARBINARY columns.
    std::vector<db::Param> bind{
        db::Param{req.username},
        db::Param{to_blob(cred.salt)},
        db::Param{to_blob(cred.verifier)},
        db::Param{static_cast<std::int64_t>(req.gm_level)},
    };
    // email is NULLable (§4.1): bind NULL when empty, else the string.
    if (req.email.empty()) {
        bind.push_back(db::Param{std::monostate{}});
    } else {
        bind.push_back(db::Param{req.email});
    }

    try {
        db::Result r = conn.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier, gm_level, email) "
            "VALUES (?, ?, ?, ?, ?)",
            bind);
        return CreateResult{r.last_insert_id, std::move(cred)};
    } catch (const db::DbError& e) {
        if (e.code() == kErDupEntry) {
            throw DuplicateUsername(req.username);
        }
        throw;
    }
}

bool set_gm_level(db::Connection& conn, const std::string& username,
                  std::uint8_t gm_level) {
    if (username.empty()) {
        throw std::invalid_argument("username must not be empty");
    }
    // Parameterized UPDATE — username + level bind through placeholders, never
    // concatenated (meridian-db safety rule). affected_rows==1 => the account
    // existed and was updated; 0 => no such username.
    db::Result r = conn.execute(
        "UPDATE account SET gm_level = ? WHERE username = ?",
        {db::Param{static_cast<std::int64_t>(gm_level)}, db::Param{username}});
    return r.affected_rows == 1;
}

}  // namespace meridian::account
