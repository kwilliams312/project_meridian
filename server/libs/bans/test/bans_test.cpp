// SPDX-License-Identifier: Apache-2.0
//
// meridian-bans — moderation model test (OPS-02c, #419; epic #21).
//
// CLEAN-ROOM: written from meridian/bans/bans.h + the auth/characters DB schema.
// No GPL source consulted (CONTRIBUTING.md).
//
// TWO HALVES:
//   PURE (always): ip_of_peer, parse_duration_seconds, parse_kind — no DB.
//   DB (MERIDIAN_DB_* set, else SKIP): the active-ban queries + issuance helpers
//   over the real account_ban / ip_ban / character_ban tables, proving:
//     * a permanent + a future-dated ban read as ACTIVE;
//     * an already-EXPIRED ban does NOT (the SQL UTC expiry predicate);
//     * the issuance helpers write a matching, immediately-active row.
//   character_mute is exercised over a self-created standalone table (its real
//   home is the characters DB; the worldd chat IT proves the end-to-end drop).

#include "meridian/bans/bans.h"

#include "meridian/db/connection.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace bans = meridian::bans;
namespace db = meridian::db;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
const char* env(const char* k) { return std::getenv(k); }

void test_pure() {
    std::printf("[bans] pure helpers (ip parse / duration / kind)\n");

    check("ip_of_peer: ipv4 strips :port",
          bans::ip_of_peer("203.0.113.7:51000") == "203.0.113.7");
    check("ip_of_peer: bracketed ipv6 strips [] + port",
          bans::ip_of_peer("[2001:db8::1]:51000") == "2001:db8::1");
    check("ip_of_peer: bare ipv6 (no port) returned whole",
          bans::ip_of_peer("2001:db8::1") == "2001:db8::1");
    check("ip_of_peer: no port returned whole",
          bans::ip_of_peer("10.0.0.5") == "10.0.0.5");
    check("ip_of_peer: empty -> empty", bans::ip_of_peer("").empty());

    check("duration: bare seconds", bans::parse_duration_seconds("3600") == 3600u);
    check("duration: 30s", bans::parse_duration_seconds("30s") == 30u);
    check("duration: 15m", bans::parse_duration_seconds("15m") == 900u);
    check("duration: 2h", bans::parse_duration_seconds("2h") == 7200u);
    check("duration: 7d", bans::parse_duration_seconds("7d") == 604800u);
    check("duration: non-numeric -> nullopt (reason word)",
          !bans::parse_duration_seconds("spamming").has_value());
    check("duration: zero -> nullopt", !bans::parse_duration_seconds("0").has_value());
    check("duration: empty -> nullopt", !bans::parse_duration_seconds("").has_value());
    check("duration: bare unit -> nullopt", !bans::parse_duration_seconds("h").has_value());

    check("kind: account", bans::parse_kind("account") == bans::Kind::kAccount);
    check("kind: char alias", bans::parse_kind("char") == bans::Kind::kCharacter);
    check("kind: character", bans::parse_kind("character") == bans::Kind::kCharacter);
    check("kind: ip", bans::parse_kind("ip") == bans::Kind::kIp);
    check("kind: unknown -> nullopt", !bans::parse_kind("nonsense").has_value());
}

// Insert a throwaway account (account_ban.account_id FKs account.id). Returns id.
std::uint64_t seed_account(db::Connection& c, const std::string& username) {
    c.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
    db::Bytes_t salt(32, 0x11);
    db::Bytes_t verifier(32, 0x22);
    db::Result r = c.execute(
        "INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
        {db::Param{username}, db::Param{salt}, db::Param{verifier}});
    return r.last_insert_id;
}

void test_db(db::Connection& c) {
    std::printf("[bans] DB: active-ban queries + issuance + expiry\n");

    const std::string uname = "bans_it_" + std::to_string(std::rand());
    const std::uint64_t acct = seed_account(c, uname);
    check("DB setup: account seeded", acct > 0);

    // --- account ban: name resolution + not-banned baseline -------------------
    check("account_id_for resolves the seeded account",
          bans::account_id_for(c, uname) == acct);
    check("clean account is not banned", !bans::account_ban(c, acct).has_value());

    // --- issue a PERMANENT account ban, assert active -------------------------
    bans::ban_account(c, acct, "griefing", /*issued_by=*/0, /*perm=*/std::nullopt);
    {
        auto a = bans::account_ban(c, acct);
        check("permanent account ban is active", a.has_value());
        check("permanent account ban carries its reason + is permanent",
              a && a->reason == "griefing" && a->permanent);
    }

    // --- a DIFFERENT, already-EXPIRED account ban is NOT active ----------------
    const std::string uname2 = "bans_it_exp_" + std::to_string(std::rand());
    const std::uint64_t acct2 = seed_account(c, uname2);
    c.execute(
        "INSERT INTO account_ban (account_id, expires_at, reason) "
        "VALUES (?, UTC_TIMESTAMP() - INTERVAL 1 HOUR, 'lapsed')",
        {db::Param{static_cast<std::int64_t>(acct2)}});
    check("an expired account ban does NOT block", !bans::account_ban(c, acct2).has_value());

    // A FUTURE-dated ban issued via the helper reads active + time-boxed.
    bans::ban_account(c, acct2, "temp", /*issued_by=*/acct, /*dur=*/3600u);
    {
        auto a = bans::account_ban(c, acct2);
        check("a future-dated account ban is active", a.has_value());
        check("a time-boxed ban is NOT marked permanent", a && !a->permanent);
    }

    // --- ip ban ---------------------------------------------------------------
    const std::string ip = "198.51.100." + std::to_string(std::rand() % 254 + 1);
    check("clean ip is not banned", !bans::ip_ban(c, ip).has_value());
    bans::ban_ip(c, ip, "botting", /*issued_by=*/0, /*perm=*/std::nullopt);
    check("banned ip reads active", bans::ip_ban(c, ip).has_value());
    // Expired ip ban does not block.
    const std::string ip2 = "198.51.100." + std::to_string(std::rand() % 254 + 1) + "9";
    c.execute(
        "INSERT INTO ip_ban (target, expires_at, reason) "
        "VALUES (?, UTC_TIMESTAMP() - INTERVAL 5 MINUTE, 'lapsed')",
        {db::Param{ip2}});
    check("an expired ip ban does NOT block", !bans::ip_ban(c, ip2).has_value());

    // --- character ban (no FK on character_id — any id is fine) ---------------
    const std::uint64_t cid = 900000u + static_cast<std::uint64_t>(std::rand() % 1000);
    check("clean character is not banned", !bans::character_ban(c, cid).has_value());
    bans::ban_character(c, cid, "exploiting", /*issued_by=*/0, /*perm=*/std::nullopt);
    check("banned character reads active", bans::character_ban(c, cid).has_value());

    // --- character mute over a self-created standalone table ------------------
    // (character_mute's real home is the characters DB; here we exercise the
    // library's query/insert/expiry against a matching standalone table.)
    c.execute("DROP TABLE IF EXISTS character_mute");
    c.execute(
        "CREATE TABLE character_mute ("
        " id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
        " char_id BIGINT UNSIGNED NOT NULL,"
        " expires_at DATETIME NULL,"
        " reason VARCHAR(255) NOT NULL,"
        " muted_by BIGINT UNSIGNED NULL,"
        " created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " PRIMARY KEY (id), KEY idx_cm_char (char_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    const std::uint64_t mchar = 700001u;
    check("clean character is not muted", !bans::character_mute(c, mchar).has_value());
    bans::mute_character(c, mchar, "spamming", /*issued_by=*/acct, /*dur=*/3600u);
    {
        auto m = bans::character_mute(c, mchar);
        check("muted character reads active", m.has_value());
        check("time-boxed mute carries reason + not permanent",
              m && m->reason == "spamming" && !m->permanent);
    }
    // Expired mute does not block.
    const std::uint64_t mchar2 = 700002u;
    c.execute(
        "INSERT INTO character_mute (char_id, expires_at, reason) "
        "VALUES (?, UTC_TIMESTAMP() - INTERVAL 1 MINUTE, 'lapsed')",
        {db::Param{static_cast<std::int64_t>(mchar2)}});
    check("an expired mute does NOT block", !bans::character_mute(c, mchar2).has_value());

    // --- cleanup (account_ban/ip_ban rows cascade / are dropped) --------------
    c.execute("DROP TABLE IF EXISTS character_mute");
    c.execute("DELETE FROM ip_ban WHERE target IN (?, ?)", {db::Param{ip}, db::Param{ip2}});
    c.execute("DELETE FROM character_ban WHERE character_id = ?",
              {db::Param{static_cast<std::int64_t>(cid)}});
    c.execute("DELETE FROM account WHERE username IN (?, ?)",
              {db::Param{uname}, db::Param{uname2}});  // account_ban cascades
}

}  // namespace

int main() {
    std::printf("meridian-bans test (OPS-02c #419)\n\n");
    test_pure();

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("\nSKIP: no MERIDIAN_DB_* configured — DB half skipped (pure half ran)\n");
    } else {
        try {
            db::Connection c(p);
            test_db(c);
        } catch (const db::DbError& e) {
            std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
            ++g_fail;
        } catch (const std::exception& e) {
            std::printf("  FAIL  exception: %s\n", e.what());
            ++g_fail;
        }
    }

    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
