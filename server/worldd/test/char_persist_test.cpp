// SPDX-License-Identifier: Apache-2.0
//
// worldd cross-session character-persistence regression test (#543; guards the
// #341 scenario). Follow-up to the #341 fix (verified resolved): a character
// created over ONE connection must still be there, owned by the same account,
// when a SEPARATE, freshly-opened connection reads it back from the SAME durable
// DB — exactly the "create a character, reopen the client → gone" bug.
//
// CLEAN-ROOM: written from the server SAD (§4.2/§4.4 characters DB + soft-ref
// rule, §5.3 IF-3 session handoff, decision D-35 server-authoritative enter-world)
// and the meridian-characters / worldd public APIs only. No GPL source consulted
// (CONTRIBUTING).
//
// WHY A SECOND TEST (the gap #543 closes): the existing char_mgmt_test does its
// whole create→list round-trip over ONE authenticated session / ONE DB handle, so
// a character that only lived in that connection's session state (never actually
// committed to durable storage) would still pass it. #341 was precisely that
// class of bug — state that survived within a session but not across a reconnect.
// This test proves DURABILITY across DISTINCT connections: it CREATEs on
// connection A, DROPS A's handle, then opens a brand-new connection B and asserts
// the row is readable there — via the REAL production read paths a reconnecting
// worldd session uses (meridian::characters::list_characters for CHAR_LIST and
// meridian::worldd::load_owned_character for the ENTER_WORLD ownership load).
//
// It deliberately exercises the production API on BOTH sides rather than
// hand-rolled SQL, so it tests the exact code a real client's char-select /
// enter-world round-trip drives. It needs no auth account/grant: all three code
// paths touch only the `character` table (account_id is an opaque soft-ref token,
// §4.4), so the test seeds its own randomised account_id and cleans up after.
//
// DB-GATED: reads MERIDIAN_DB_* env and SKIPS (exit 0) when none are set, so it is
// inert in the plain server ctest and runs for real only in the worldd-session CI
// job (or locally via scripts/dev/test.sh --db). It CREATE-TABLE-IF-NOT-EXISTS the
// `character` table (standalone — no outgoing FK, §4.4), matching char_mgmt_test /
// characters_test, so the same DB that holds the auth schema serves as the
// characters DB here.

#include "characters.h"
#include "meridian/db/connection.h"
#include "roster.h"
#include "world_session.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace meridian;
namespace chr = meridian::characters;
namespace mw = meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

// Standalone `character` table DDL — mirrors server/db/characters/migrations/
// 0001_init_characters.up.sql (and char_mgmt_test / characters_test). The
// character table has NO outgoing FK (account_id is a soft ref, §4.4), so
// CREATE ... IF NOT EXISTS is a no-op when the real migration already loaded it
// and stands alone otherwise.
constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id            BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,"
    "  account_id    BIGINT UNSIGNED   NOT NULL,"
    "  name          VARCHAR(32)       NOT NULL,"
    "  race          TINYINT UNSIGNED  NOT NULL,"
    "  class         TINYINT UNSIGNED  NOT NULL,"
    "  appearance    JSON              NULL,"
    "  level         SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  xp            INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  money         BIGINT UNSIGNED   NOT NULL DEFAULT 0,"
    "  map_id        INT UNSIGNED      NOT NULL,"
    "  instance_id   INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  pos_x         FLOAT             NOT NULL,"
    "  pos_y         FLOAT             NOT NULL,"
    "  pos_z         FLOAT             NOT NULL,"
    "  pos_o         FLOAT             NOT NULL DEFAULT 0,"
    "  played_time   INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  logout_at     DATETIME          NULL,"
    "  save_epoch    BIGINT            NOT NULL DEFAULT 0,"
    "  created_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_name (name),"
    "  KEY idx_character_account (account_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

// True iff `list` contains a character with the given id.
bool has_id(const std::vector<chr::CharacterSummary>& list, std::uint64_t id) {
    for (const auto& c : list) {
        if (c.id == id) return true;
    }
    return false;
}

}  // namespace

int main() {
    std::printf("worldd cross-session character-persistence test (#543 / #341)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — cross-session "
                    "persistence checks skipped (set MERIDIAN_DB_SOCKET or "
                    "MERIDIAN_DB_HOST + MERIDIAN_DB_USER)\n");
        return 0;
    }

    // Randomised, distinct account_ids + name so repeated/parallel local runs never
    // collide (std::rand() is unseeded — stable across runs — so the pre-cleanup
    // below clears any leftover row by NAME, which is GLOBAL via uq_character_name).
    // account_other is a synthetic FOREIGN owner token used only for the negative
    // ownership assertion (soft ref, §4.4 — no auth row needed).
    const int salt = std::rand();
    const std::uint64_t account_a = 4'600'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_other = 4'700'000'000ULL + static_cast<unsigned>(salt);
    const std::string name_a = "Cp_" + std::to_string(salt) + "_a";

    std::uint64_t minted = 0;

    try {
        // === Connection A: prepare fixture + CREATE the character, then CLOSE. ===
        // Scoped so A's db handle is DESTROYED (connection dropped) before B opens
        // — this is the whole point: B must read committed durable state, not any
        // per-connection/session residue. A reconnecting worldd session gets a
        // fresh connection exactly like B does.
        {
            db::Connection a(p);
            std::printf("connection A: connected to MariaDB\n");
            a.execute(kCharacterDdl);

            // Clear any stray row from a prior aborted run (uq_character_name is
            // global, so a leftover name_a would make the create below fail dup).
            a.execute("DELETE FROM `character` WHERE name = ? OR account_id IN (?, ?)",
                      {db::Param{name_a},
                       db::Param{std::to_string(account_a)},
                       db::Param{std::to_string(account_other)}});

            chr::CreateRequest req;
            req.account_id = account_a;
            req.name = name_a;
            req.race = static_cast<std::uint8_t>(chr::Race::kSylvane);
            req.char_class = static_cast<std::uint8_t>(chr::Class::kRuncaller);
            minted = chr::create_character(a, req).character_id;
            check("A: create_character returns a server-minted id", minted > 0);
        }  // connection A closed HERE — its handle is gone before B opens.

        // === Connection B: a SEPARATELY-opened, fresh handle. ===================
        // Everything below reads through the REAL production paths a reconnecting
        // client's worldd session drives; NONE of the state created on A is in
        // scope, so a pass means the row is genuinely durable.
        db::Connection b(p);
        std::printf("connection B: opened a FRESH connection\n");

        // 1. CHAR_LIST path (meridian::characters::list_characters) on connection B
        //    finds the character created on A — the exact #341 regression guard.
        {
            std::vector<chr::CharacterSummary> listed = chr::list_characters(b, account_a);
            check("B: list on the fresh connection shows exactly one character",
                  listed.size() == 1);
            check("B: the listed character is the id minted on connection A",
                  has_id(listed, minted));
            if (listed.size() == 1) {
                check("B: listed id matches the minted id", listed[0].id == minted);
                check("B: listed name matches what A created", listed[0].name == name_a);
                check("B: listed account_id matches the creating account",
                      listed[0].account_id == account_a);
                check("B: listed race round-trips across the connection",
                      listed[0].race == static_cast<std::uint8_t>(chr::Race::kSylvane));
                check("B: listed class round-trips across the connection",
                      listed[0].char_class ==
                          static_cast<std::uint8_t>(chr::Class::kRuncaller));
            }
        }

        // 2. ENTER_WORLD ownership path (meridian::worldd::load_owned_character) on
        //    connection B succeeds for the owning account — server-authoritative
        //    entry works on the reconnect (D-35). This is the second half of the
        //    #341 scenario ("... and ENTER_WORLD on its id succeeds").
        {
            std::optional<mw::LoadedCharacter> owned =
                mw::load_owned_character(b, account_a, minted);
            check("B: ENTER_WORLD load for the owning account succeeds",
                  owned.has_value());
            if (owned) {
                check("B: loaded char_guid matches the minted id",
                      owned->char_guid == minted);
                check("B: loaded name matches what A created", owned->name == name_a);
                check("B: loaded class round-trips",
                      owned->class_id ==
                          static_cast<std::uint8_t>(chr::Class::kRuncaller));
            }
        }

        // 3. Negative ownership: the SAME id under a DIFFERENT account loads
        //    nothing across the connection — the account_id predicate (ownership)
        //    holds on the fresh handle too, so persistence did not leak the row to
        //    the wrong account.
        {
            std::optional<mw::LoadedCharacter> foreign =
                mw::load_owned_character(b, account_other, minted);
            check("B: ENTER_WORLD load for a FOREIGN account finds nothing "
                  "(ownership predicate holds cross-connection)",
                  !foreign.has_value());
        }

        // --- Cleanup: remove this run's row (on B — A is already closed). --------
        b.execute("DELETE FROM `character` WHERE id = ?",
                  {db::Param{std::to_string(minted)}});
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL CROSS-SESSION PERSISTENCE TESTS PASSED\n"
                            : "\n%d CROSS-SESSION PERSISTENCE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
