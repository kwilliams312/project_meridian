// SPDX-License-Identifier: Apache-2.0
//
// meridian-bans — moderation enforcement model (OPS-02c, #419; epic #21).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §4.1 (the auth DB account_ban
// / ip_ban field lists + "authd refuses a banned login, gatewayd refuses a banned
// grant"), §4.2 (per-character durable state), the server PRD §4-M1 / §6 (OPS-02
// moderation + the append-only audit streams), and story #419. No GPL source
// (CMaNGOS / TrinityCore or otherwise) consulted. See CONTRIBUTING.md.
//
// WHAT THIS IS. A small, socket-free data-access library over the ban/mute tables,
// shared by authd (login refusal), worldd (session refusal + chat-mute
// enforcement + GM `.ban`/`.mute` issuance), and any operator CLI. It is
// deliberately PURE of policy: it only reads/writes rows and answers "is this
// subject currently blocked?" — the callers own the wire/audit reactions.
//
// THE MODEL (all SQL parameterized — user input never concatenated; §backend std):
//   * account_ban / ip_ban  (auth DB, migration 0001) — a banned ACCOUNT or source
//     IP is refused at authd login and again at the worldd handshake.
//   * character_ban          (auth DB, migration 0003) — a banned CHARACTER is
//     refused at worldd ENTER_WORLD (the account may still play its others).
//   * character_mute         (characters DB, migration 0002) — a muted CHARACTER's
//     CHAT_MESSAGE is dropped at the chat router with a "you are muted" reply.
// A ban/mute with a NULL expires_at is PERMANENT; otherwise it is active only
// while expires_at is in the future. Expiry is evaluated against the DB's UTC
// clock (UTC_TIMESTAMP()) in the query, so an elapsed ban/mute simply stops
// matching — no sweep is required for correctness.

#ifndef MERIDIAN_BANS_BANS_H
#define MERIDIAN_BANS_BANS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "meridian/db/connection.h"

namespace meridian::bans {

// The subject a ban/mute targets — the GM-command routing selector + the audit
// `target` prefix ("account:"/"character:"/"ip:").
enum class Kind {
    kAccount,
    kCharacter,
    kIp,
};

// The stable lowercase name of a Kind ("account"/"character"/"ip"). Used for the
// GM `.ban <kind>` keyword parse and the audit target string.
const char* kind_name(Kind kind);

// Parse a `.ban` kind keyword ("account"/"char"/"character"/"ip") to a Kind. The
// short "char" alias is accepted. Returns nullopt for anything else.
std::optional<Kind> parse_kind(std::string_view keyword);

// One currently-active ban/mute row (the row a query matched). `permanent` mirrors
// expires_at IS NULL; `expires_at` is the UTC "YYYY-MM-DD HH:MM:SS" string when
// time-boxed (empty when permanent).
struct Active {
    std::uint64_t id = 0;
    std::string reason;
    bool permanent = true;
    std::string expires_at;  // empty iff permanent
};

// ---------------------------------------------------------------------------
// Pure helpers (no DB) — unit-testable without a connection.
// ---------------------------------------------------------------------------

// Extract the IP portion of a meridian::net::Session::peer() string ("ip:port").
// Handles IPv4 ("203.0.113.7:51000" -> "203.0.113.7") and bracketed IPv6
// ("[2001:db8::1]:51000" -> "2001:db8::1"). If there is no recognizable port
// suffix the input is returned unchanged (best effort — the peer string is only
// used for an exact-match IP-ban lookup + audit).
std::string ip_of_peer(std::string_view peer);

// Parse a duration token for a `.ban`/`.mute` argument: a bare count of seconds
// ("3600") or a suffixed value ("30s"/"15m"/"2h"/"7d"). Returns the number of
// seconds, or nullopt when `s` is not a valid duration form — so a command parser
// can treat an unrecognised token as the first word of the reason instead. Zero
// and overflow are rejected (nullopt).
std::optional<std::uint64_t> parse_duration_seconds(std::string_view s);

// ---------------------------------------------------------------------------
// Active-ban queries (auth DB). Return the blocking row, or nullopt when clear.
// A permanent ban is preferred, else the latest-expiring active one.
// ---------------------------------------------------------------------------
std::optional<Active> account_ban(db::Connection& auth_db, std::uint64_t account_id);
std::optional<Active> ip_ban(db::Connection& auth_db, const std::string& ip);
std::optional<Active> character_ban(db::Connection& auth_db, std::uint64_t character_id);

// Active-mute query (characters DB). Returns the blocking mute row, or nullopt.
std::optional<Active> character_mute(db::Connection& char_db, std::uint64_t character_id);

// ---------------------------------------------------------------------------
// Name resolution — map a GM-typed name to its surrogate id for issuance.
// ---------------------------------------------------------------------------
// account.username -> account.id (auth DB). nullopt if no such account.
std::optional<std::uint64_t> account_id_for(db::Connection& auth_db,
                                            const std::string& username);
// character.name -> character.id (characters DB). nullopt if no such character.
std::optional<std::uint64_t> character_id_for(db::Connection& char_db,
                                              const std::string& name);

// ---------------------------------------------------------------------------
// Issuance (INSERT). `duration_seconds` nullopt => permanent (expires_at NULL);
// otherwise expires_at = now + duration (UTC). `issued_by` is the acting GM's
// account.id, 0 => system/automated (stored as NULL). All parameterized.
// ---------------------------------------------------------------------------
void ban_account(db::Connection& auth_db, std::uint64_t account_id,
                 const std::string& reason, std::uint64_t issued_by,
                 std::optional<std::uint64_t> duration_seconds);
void ban_ip(db::Connection& auth_db, const std::string& target,
            const std::string& reason, std::uint64_t issued_by,
            std::optional<std::uint64_t> duration_seconds);
void ban_character(db::Connection& auth_db, std::uint64_t character_id,
                   const std::string& reason, std::uint64_t issued_by,
                   std::optional<std::uint64_t> duration_seconds);
void mute_character(db::Connection& char_db, std::uint64_t character_id,
                    const std::string& reason, std::uint64_t issued_by,
                    std::optional<std::uint64_t> duration_seconds);

}  // namespace meridian::bans

#endif  // MERIDIAN_BANS_BANS_H
