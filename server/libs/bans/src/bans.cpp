// SPDX-License-Identifier: Apache-2.0
//
// meridian-bans — moderation enforcement model implementation (OPS-02c, #419).
// See meridian/bans/bans.h for the design + provenance.

#include "meridian/bans/bans.h"

#include <cctype>
#include <cstdlib>
#include <ctime>

namespace meridian::bans {
namespace {

// A single active-ban SELECT, shared by the three auth-DB ban tables (they have
// the same {id, expires_at, reason} shape). `id_col` is the subject column name
// (account_id / character_id) — a FIXED, code-supplied identifier, NEVER user
// input, so it is safe to interpolate; the subject VALUE binds through `?`.
db::Result select_active_ban(db::Connection& db, const char* table,
                             const char* id_col, const db::Param& subject) {
    std::string sql = "SELECT id, reason, expires_at FROM ";
    sql += table;
    sql += " WHERE ";
    sql += id_col;
    sql +=
        " = ? AND (expires_at IS NULL OR expires_at > UTC_TIMESTAMP()) "
        "ORDER BY (expires_at IS NULL) DESC, expires_at DESC LIMIT 1";
    return db.execute(sql, {subject});
}

std::optional<Active> row_to_active(const db::Result& r) {
    if (r.rows.size() != 1) return std::nullopt;
    const db::Row& row = r.rows[0];
    Active a;
    a.id = row[0].has_value() ? std::strtoull(row[0]->c_str(), nullptr, 10) : 0;
    a.reason = row[1].has_value() ? *row[1] : "";
    a.permanent = !row[2].has_value();
    a.expires_at = row[2].has_value() ? *row[2] : "";
    return a;
}

// Format "YYYY-MM-DD HH:MM:SS" (UTC) `seconds` from now, for an expires_at INSERT.
std::string utc_expires_in(std::uint64_t seconds) {
    std::time_t exp = std::time(nullptr) + static_cast<std::time_t>(seconds);
    std::tm tm_utc{};
    gmtime_r(&exp, &tm_utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
    return std::string(buf);
}

// The expires_at bind value for a duration: a UTC datetime string, or NULL
// (std::monostate) for a permanent ban/mute.
db::Param expires_param(std::optional<std::uint64_t> duration_seconds) {
    if (duration_seconds.has_value()) return db::Param{utc_expires_in(*duration_seconds)};
    return db::Param{};  // std::monostate => SQL NULL (permanent)
}

// The banned_by / muted_by bind value: the acting account id, or NULL for 0
// (system/automated), matching the "NULL = system" column semantics.
db::Param actor_param(std::uint64_t issued_by) {
    if (issued_by == 0) return db::Param{};  // NULL
    return db::Param{static_cast<std::int64_t>(issued_by)};
}

std::optional<std::uint64_t> select_id(db::Connection& db, const char* sql,
                                       const std::string& value) {
    db::Result r = db.execute(sql, {db::Param{value}});
    if (r.rows.size() != 1 || !r.rows[0][0].has_value()) return std::nullopt;
    return std::strtoull(r.rows[0][0]->c_str(), nullptr, 10);
}

}  // namespace

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::kAccount:   return "account";
        case Kind::kCharacter: return "character";
        case Kind::kIp:        return "ip";
    }
    return "account";
}

std::optional<Kind> parse_kind(std::string_view keyword) {
    if (keyword == "account") return Kind::kAccount;
    if (keyword == "char" || keyword == "character") return Kind::kCharacter;
    if (keyword == "ip") return Kind::kIp;
    return std::nullopt;
}

std::string ip_of_peer(std::string_view peer) {
    if (peer.empty()) return std::string();
    // Bracketed IPv6: "[addr]:port" -> "addr".
    if (peer.front() == '[') {
        const std::size_t close = peer.find(']');
        if (close != std::string_view::npos) return std::string(peer.substr(1, close - 1));
        return std::string(peer);
    }
    // A single ':' means "ipv4:port"; multiple colons is a bare (unbracketed)
    // IPv6 literal with no port suffix — return it whole.
    const std::size_t first = peer.find(':');
    if (first == std::string_view::npos) return std::string(peer);
    if (peer.find(':', first + 1) != std::string_view::npos) return std::string(peer);
    return std::string(peer.substr(0, first));
}

std::optional<std::uint64_t> parse_duration_seconds(std::string_view s) {
    if (s.empty()) return std::nullopt;
    // Optional single trailing unit suffix; default unit is seconds.
    std::uint64_t mult = 1;
    std::string_view digits = s;
    const char last = s.back();
    if (last == 's' || last == 'm' || last == 'h' || last == 'd') {
        switch (last) {
            case 's': mult = 1; break;
            case 'm': mult = 60; break;
            case 'h': mult = 3600; break;
            case 'd': mult = 86400; break;
        }
        digits = s.substr(0, s.size() - 1);
    }
    if (digits.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t next = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (next < value) return std::nullopt;  // overflow
        value = next;
    }
    if (value == 0) return std::nullopt;  // a zero-length ban is not a duration
    // Guard the multiply against overflow.
    if (value > (0xFFFF'FFFF'FFFF'FFFFULL / mult)) return std::nullopt;
    return value * mult;
}

std::optional<Active> account_ban(db::Connection& auth_db, std::uint64_t account_id) {
    return row_to_active(select_active_ban(
        auth_db, "account_ban", "account_id",
        db::Param{static_cast<std::int64_t>(account_id)}));
}

std::optional<Active> ip_ban(db::Connection& auth_db, const std::string& ip) {
    if (ip.empty()) return std::nullopt;
    return row_to_active(
        select_active_ban(auth_db, "ip_ban", "target", db::Param{ip}));
}

std::optional<Active> character_ban(db::Connection& auth_db, std::uint64_t character_id) {
    return row_to_active(select_active_ban(
        auth_db, "character_ban", "character_id",
        db::Param{static_cast<std::int64_t>(character_id)}));
}

std::optional<Active> character_mute(db::Connection& char_db, std::uint64_t character_id) {
    return row_to_active(select_active_ban(
        char_db, "character_mute", "char_id",
        db::Param{static_cast<std::int64_t>(character_id)}));
}

std::optional<std::uint64_t> account_id_for(db::Connection& auth_db,
                                            const std::string& username) {
    return select_id(auth_db, "SELECT id FROM account WHERE username = ?", username);
}

std::optional<std::uint64_t> character_id_for(db::Connection& char_db,
                                              const std::string& name) {
    return select_id(char_db, "SELECT id FROM `character` WHERE name = ?", name);
}

void ban_account(db::Connection& auth_db, std::uint64_t account_id,
                 const std::string& reason, std::uint64_t issued_by,
                 std::optional<std::uint64_t> duration_seconds) {
    auth_db.execute(
        "INSERT INTO account_ban (account_id, expires_at, reason, banned_by) "
        "VALUES (?, ?, ?, ?)",
        {db::Param{static_cast<std::int64_t>(account_id)},
         expires_param(duration_seconds), db::Param{reason}, actor_param(issued_by)});
}

void ban_ip(db::Connection& auth_db, const std::string& target,
            const std::string& reason, std::uint64_t issued_by,
            std::optional<std::uint64_t> duration_seconds) {
    auth_db.execute(
        "INSERT INTO ip_ban (target, expires_at, reason, banned_by) "
        "VALUES (?, ?, ?, ?)",
        {db::Param{target}, expires_param(duration_seconds), db::Param{reason},
         actor_param(issued_by)});
}

void ban_character(db::Connection& auth_db, std::uint64_t character_id,
                   const std::string& reason, std::uint64_t issued_by,
                   std::optional<std::uint64_t> duration_seconds) {
    auth_db.execute(
        "INSERT INTO character_ban (character_id, expires_at, reason, banned_by) "
        "VALUES (?, ?, ?, ?)",
        {db::Param{static_cast<std::int64_t>(character_id)},
         expires_param(duration_seconds), db::Param{reason}, actor_param(issued_by)});
}

void mute_character(db::Connection& char_db, std::uint64_t character_id,
                    const std::string& reason, std::uint64_t issued_by,
                    std::optional<std::uint64_t> duration_seconds) {
    char_db.execute(
        "INSERT INTO character_mute (char_id, expires_at, reason, muted_by) "
        "VALUES (?, ?, ?, ?)",
        {db::Param{static_cast<std::int64_t>(character_id)},
         expires_param(duration_seconds), db::Param{reason}, actor_param(issued_by)});
}

}  // namespace meridian::bans
