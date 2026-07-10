// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters integration test (CHR-01 stub, D-11; issue #85).
//
// Needs a live MariaDB; reads connection params from MERIDIAN_DB_* env (the same
// var names as meridian-db's test) and SKIPS (exit 0) when none are set, so it is
// inert in the plain server build's ctest and runs for real only in the DB CI
// job or locally via `scripts/dev/test.sh --db`.
//
// What it proves end-to-end against the characters DB `character` table:
//   1. create -> list shows the new character (fields round-trip).
//   2. a duplicate name is refused (DuplicateName / uq_character_name).
//   3. an invalid race id is refused (InvalidRace / M0-frozen roster).
//   4. an invalid class id is refused (InvalidClass / M0-frozen roster).
//   5. deleting ANOTHER account's character is refused (ownership predicate) —
//      the row survives.
//   6. deleting your OWN character removes it (list no longer shows it).
//   7. a > INT64_MAX account_id round-trips through create + list, proving the
//      BIGINT-UNSIGNED decimal-string binding (the meridian-db signed-bind
//      gotcha) is handled.
//
// The test is self-contained like the account test: it CREATE-TABLE-IF-NOT-EXISTS
// the `character` table (standalone — the character table has no outgoing FK, so
// it stands alone in any schema), operates only on its own randomised
// account_ids, and cleans those rows up at the end. It never drops a table.
//
// Clean-room, original code; no GPL source consulted (CONTRIBUTING.md).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "characters.h"
#include "meridian/db/connection.h"
#include "roster.h"

using namespace meridian;

namespace {
int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Standalone `character` table DDL — mirrors the columns of
// server/db/characters/migrations/0001_init_characters.up.sql. The character
// table has NO outgoing foreign keys (account_id is a soft ref, §4.4), so this
// stands alone in a fresh schema; CREATE ... IF NOT EXISTS is a no-op when the
// real migration already loaded it (the --db harness loads it into
// meridian_characters).
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
bool has_id(const std::vector<characters::CharacterSummary>& list, std::uint64_t id) {
    for (const auto& c : list) {
        if (c.id == id) return true;
    }
    return false;
}
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

    // Randomised, distinct account_ids + name prefix so repeated/parallel local
    // runs never collide. account_big is > INT64_MAX to exercise the unsigned
    // decimal-string binding path (scenario 7).
    const int salt = std::rand();
    const std::uint64_t account_a = 4'000'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_b = 4'100'000'000ULL + static_cast<unsigned>(salt);
    // account_c / account_d exercise the one-character-per-account cap (#329):
    // account_c creates one then is refused a second; account_d (a different
    // account) can still create its first.
    const std::uint64_t account_c = 4'200'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_d = 4'300'000'000ULL + static_cast<unsigned>(salt);
    // account_e / account_f exercise the appearance bounds rule (contract ① §9):
    // account_e creates WITHOUT appearance (server default {1,1,1,1}); account_f
    // creates with an out-of-bounds record (version!=1, preset id 0) that clamps.
    const std::uint64_t account_e = 4'400'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_f = 4'500'000'000ULL + static_cast<unsigned>(salt);
    const std::uint64_t account_big =
        18'000'000'000'000'000'000ULL + static_cast<unsigned>(salt % 100000);
    const std::string name_a = "Chr_" + std::to_string(salt) + "_a";
    const std::string name_dup = name_a;  // same name -> duplicate
    const std::string name_b = "Chr_" + std::to_string(salt) + "_b";
    const std::string name_big = "Chr_" + std::to_string(salt) + "_big";
    const std::string name_c1 = "Chr_" + std::to_string(salt) + "_c1";
    const std::string name_c2 = "Chr_" + std::to_string(salt) + "_c2";
    const std::string name_d = "Chr_" + std::to_string(salt) + "_d";
    const std::string name_e = "Chr_" + std::to_string(salt) + "_e";
    const std::string name_f = "Chr_" + std::to_string(salt) + "_f";

    auto cleanup = [&](db::Connection& db) {
        for (std::uint64_t acct :
             {account_a, account_b, account_c, account_d, account_e, account_f,
              account_big}) {
            db.execute("DELETE FROM `character` WHERE account_id = ?",
                       {db::Param{std::to_string(acct)}});
        }
    };

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        cleanup(db);  // clear any stray rows from a prior aborted run

        // ---- 1. create -> list shows it ------------------------------------
        characters::CreateRequest req;
        req.account_id = account_a;
        req.name = name_a;
        req.race = static_cast<std::uint8_t>(characters::Race::kArdent);
        req.char_class = static_cast<std::uint8_t>(characters::Class::kVanguard);
        // Explicit, in-bounds appearance record (contract ① §5.2): a non-default
        // preset per channel so "list returns what create stored" is meaningful.
        req.appearance = characters::AppearanceRecord{/*version=*/1, /*hair=*/2,
                                                      /*face=*/3, /*skin=*/4};
        characters::CreateResult created = characters::create_character(db, req);
        check("create returns a server-minted id", created.character_id > 0);

        std::vector<characters::CharacterSummary> listed =
            characters::list_characters(db, account_a);
        check("list shows exactly one character for the account", listed.size() == 1);
        if (listed.size() == 1) {
            const auto& c = listed[0];
            check("listed id matches created id", c.id == created.character_id);
            check("listed account_id matches", c.account_id == account_a);
            check("listed name matches", c.name == name_a);
            check("listed race matches",
                  c.race == static_cast<std::uint8_t>(characters::Race::kArdent));
            check("listed class matches",
                  c.char_class == static_cast<std::uint8_t>(characters::Class::kVanguard));
            check("new character starts at level 1", c.level == 1);
            // Explicit appearance round-trips exactly (stored as JSON, parsed back).
            check("listed appearance version round-trips", c.appearance.version == 1);
            check("listed appearance hair round-trips", c.appearance.hair == 2);
            check("listed appearance face round-trips", c.appearance.face == 3);
            check("listed appearance skin round-trips", c.appearance.skin == 4);
        }

        // ---- 2. duplicate name is refused ----------------------------------
        {
            characters::CreateRequest dup;
            dup.account_id = account_b;  // different account, same name
            dup.name = name_dup;
            dup.race = static_cast<std::uint8_t>(characters::Race::kArdent);
            dup.char_class = static_cast<std::uint8_t>(characters::Class::kRuncaller);
            bool threw = false;
            try {
                characters::create_character(db, dup);
            } catch (const characters::DuplicateName&) {
                threw = true;
            }
            check("duplicate name refused (DuplicateName)", threw);
        }

        // ---- 3. invalid race is refused ------------------------------------
        {
            characters::CreateRequest bad;
            bad.account_id = account_b;
            bad.name = name_b;
            bad.race = 0;  // 0 is reserved-invalid in the M0-frozen roster
            bad.char_class = static_cast<std::uint8_t>(characters::Class::kVanguard);
            bool threw = false;
            try {
                characters::create_character(db, bad);
            } catch (const characters::InvalidRace&) {
                threw = true;
            }
            check("invalid race refused (InvalidRace)", threw);
        }

        // ---- 4. invalid class is refused -----------------------------------
        {
            characters::CreateRequest bad;
            bad.account_id = account_b;
            bad.name = name_b;
            bad.race = static_cast<std::uint8_t>(characters::Race::kArdent);
            bad.char_class = static_cast<std::uint8_t>(characters::kClassCount + 1);  // out of range
            bool threw = false;
            try {
                characters::create_character(db, bad);
            } catch (const characters::InvalidClass&) {
                threw = true;
            }
            check("invalid class refused (InvalidClass)", threw);
        }

        // ---- 5. deleting another account's character is refused ------------
        // account_a owns `created`. account_b tries to delete it -> no-op.
        {
            bool deleted = characters::delete_character(db, account_b, created.character_id);
            check("delete of another account's character refused (returns false)", !deleted);
            std::vector<characters::CharacterSummary> still =
                characters::list_characters(db, account_a);
            check("victim character still present after refused delete",
                  has_id(still, created.character_id));
        }

        // ---- 6. deleting your own character removes it ---------------------
        {
            bool deleted = characters::delete_character(db, account_a, created.character_id);
            check("delete of own character succeeds (returns true)", deleted);
            std::vector<characters::CharacterSummary> after =
                characters::list_characters(db, account_a);
            check("character gone from list after own delete", after.empty());
        }

        // ---- 7. > INT64_MAX account_id round-trips (unsigned binding) ------
        {
            characters::CreateRequest big;
            big.account_id = account_big;  // > 9.22e18, i.e. > INT64_MAX
            big.name = name_big;
            big.race = static_cast<std::uint8_t>(characters::Race::kSylvane);
            big.char_class = static_cast<std::uint8_t>(characters::Class::kMender);
            characters::CreateResult r = characters::create_character(db, big);
            check("create with > INT64_MAX account_id succeeds", r.character_id > 0);
            std::vector<characters::CharacterSummary> listed_big =
                characters::list_characters(db, account_big);
            check("list by > INT64_MAX account_id finds the character",
                  listed_big.size() == 1);
            check("> INT64_MAX account_id round-trips exactly",
                  listed_big.size() == 1 && listed_big[0].account_id == account_big);
        }

        // ---- 8. one-character-per-account cap (#329) -----------------------
        // (a) the 1st create for a fresh account succeeds; (b) a 2nd create for
        // the SAME account is refused with the cap reason (CharacterLimitReached),
        // and the account still owns exactly one character; (c) a DIFFERENT
        // account can still create its 1st character.
        {
            characters::CreateRequest first;
            first.account_id = account_c;
            first.name = name_c1;
            first.race = static_cast<std::uint8_t>(characters::Race::kArdent);
            first.char_class = static_cast<std::uint8_t>(characters::Class::kVanguard);
            characters::CreateResult r1 = characters::create_character(db, first);
            check("cap: 1st create for the account succeeds", r1.character_id > 0);

            characters::CreateRequest second;
            second.account_id = account_c;   // same account -> over the cap
            second.name = name_c2;           // unique name: the refusal is the cap
            second.race = static_cast<std::uint8_t>(characters::Race::kSylvane);
            second.char_class = static_cast<std::uint8_t>(characters::Class::kMender);
            bool capped = false;
            try {
                characters::create_character(db, second);
            } catch (const characters::CharacterLimitReached&) {
                capped = true;
            }
            check("cap: 2nd create for same account refused (CharacterLimitReached)",
                  capped);

            std::vector<characters::CharacterSummary> owned =
                characters::list_characters(db, account_c);
            check("cap: account still owns exactly one character", owned.size() == 1);

            characters::CreateRequest other;
            other.account_id = account_d;    // different account -> allowed
            other.name = name_d;
            other.race = static_cast<std::uint8_t>(characters::Race::kArdent);
            other.char_class = static_cast<std::uint8_t>(characters::Class::kRuncaller);
            characters::CreateResult rd = characters::create_character(db, other);
            check("cap: a different account can still create its 1st character",
                  rd.character_id > 0);
        }

        // ---- 9. appearance ABSENT -> server default {1,1,1,1} --------------
        // A create that supplies no appearance (the CHR-01 stub / old client)
        // stores the NULL column; list must materialise the versioned default
        // (contract ① §5.2: absent ⇒ {v:1,hair:1,face:1,skin:1}).
        {
            characters::CreateRequest noapp;
            noapp.account_id = account_e;
            noapp.name = name_e;
            noapp.race = static_cast<std::uint8_t>(characters::Race::kDolmen);
            noapp.char_class = static_cast<std::uint8_t>(characters::Class::kWarden);
            // noapp.appearance intentionally left unset (std::nullopt).
            characters::create_character(db, noapp);
            std::vector<characters::CharacterSummary> got =
                characters::list_characters(db, account_e);
            check("absent-appearance create succeeds + lists one", got.size() == 1);
            if (got.size() == 1) {
                const auto& a = got[0].appearance;
                check("absent appearance defaults version=1", a.version == 1);
                check("absent appearance defaults hair=1", a.hair == 1);
                check("absent appearance defaults face=1", a.face == 1);
                check("absent appearance defaults skin=1", a.skin == 1);
            }
        }

        // ---- 10. appearance out-of-bounds -> clamped -----------------------
        // The record is opaque-but-bounded, never gameplay-authoritative
        // (contract ① §9): version!=1 clamps to 1 (only v1 exists at M1) and a
        // preset id of 0 clamps to 1 (ids are 1-based). No new failure taxonomy —
        // the create still SUCCEEDS, the stored record is just normalised.
        {
            characters::CreateRequest oob;
            oob.account_id = account_f;
            oob.name = name_f;
            oob.race = static_cast<std::uint8_t>(characters::Race::kEmberkin);
            oob.char_class = static_cast<std::uint8_t>(characters::Class::kMender);
            oob.appearance = characters::AppearanceRecord{/*version=*/7, /*hair=*/0,
                                                          /*face=*/5, /*skin=*/0};
            characters::create_character(db, oob);
            std::vector<characters::CharacterSummary> got =
                characters::list_characters(db, account_f);
            check("out-of-bounds-appearance create still succeeds", got.size() == 1);
            if (got.size() == 1) {
                const auto& a = got[0].appearance;
                check("version!=1 clamped to 1", a.version == 1);
                check("preset id 0 (hair) clamped to 1", a.hair == 1);
                check("in-bounds preset id (face=5) preserved", a.face == 5);
                check("preset id 0 (skin) clamped to 1", a.skin == 1);
            }
        }

        cleanup(db);  // remove this run's rows
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL CHARACTER TESTS PASSED\n"
                            : "\n%d CHARACTER TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
