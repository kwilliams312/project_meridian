// SPDX-License-Identifier: Apache-2.0
//
// meridian-npc DB-backed integration test (NPC-02; issue #372).
//
// Needs a live MariaDB; reads MERIDIAN_DB_* (same var names as the other DB tests)
// and SKIPS (exit 0) when none are set, so it is inert in the DB-free ctest and
// runs for real only in the DB CI job / `scripts/dev/test.sh --db`. Modelled on the
// meridian-items DB test (item_store_it.cpp) — self-contained CREATE ... IF NOT
// EXISTS of the `character` table, operate on its own randomised character, clean
// up its row; never drops a table.
//
// What it proves end-to-end against the characters DB (the story's DB-integration
// ask):
//   1. A VALID learn (right class + level + enough copper) debits the cost from
//      `character.money` — the durable balance drops by exactly the cost — and the
//      ability is recorded as learned (in-memory for M1; see below).
//   2. EVERY rejection (wrong class, too-low level, insufficient funds, already
//      known) leaves `character.money` UNCHANGED — all-or-nothing, money never
//      moves on a rejected learn.
//   3. A second valid learn debits again; the two debits sum exactly.
//
// LEARNED-ABILITY PERSISTENCE (M1): there is no durable `character_ability` table
// yet — learned abilities are held IN MEMORY (LearnedAbilitySet), documented in
// trainer.h. So this test asserts the COPPER debit through the real DB and the
// learned state through the in-memory set; it does NOT assert a persisted ability
// row (there is none to assert at M1).
//
// Clean-room, original code (CONTRIBUTING.md).

#include <cstdio>
#include <cstdlib>
#include <string>

#include "currency.h"
#include "meridian/db/connection.h"
#include "npc_def.h"
#include "trainer.h"

using namespace meridian;
namespace mn = meridian::npc;
namespace mi = meridian::items;

namespace {

int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

db::Param sid(std::uint64_t v) { return db::Param{std::to_string(v)}; }

// The one characters-DB table this test touches. Columns mirror
// server/db/characters/migrations/0001_init_characters.up.sql (same subset the
// items DB test uses). CREATE ... IF NOT EXISTS is a no-op when the real migration
// already loaded it.
constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  account_id BIGINT UNSIGNED NOT NULL,"
    "  name VARCHAR(32) NOT NULL,"
    "  race TINYINT UNSIGNED NOT NULL,"
    "  class TINYINT UNSIGNED NOT NULL,"
    "  level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  xp INT UNSIGNED NOT NULL DEFAULT 0,"
    "  money BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    "  map_id INT UNSIGNED NOT NULL,"
    "  instance_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
    "  pos_o FLOAT NOT NULL DEFAULT 0,"
    "  played_time INT UNSIGNED NOT NULL DEFAULT 0,"
    "  logout_at DATETIME NULL,"
    "  save_epoch BIGINT NOT NULL DEFAULT 0,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id), UNIQUE KEY uq_character_name (name),"
    "  KEY idx_character_account (account_id)"
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

    const int salt = std::rand();
    const std::uint64_t account_id = 4'720'000'000ULL + static_cast<unsigned>(salt);
    const std::string cname = "Npc_" + std::to_string(salt);
    // A Vanguard (class 1) at level 5 — eligible for both placeholder trainer
    // abilities (the Vanguard-only strike at level 2 / 50c, and the any-class heal
    // at level 5 / 120c). Starts with 200 copper: enough for the strike, and then
    // NOT enough for the heal until we top up — so the same character exercises a
    // successful debit AND an insufficient-funds rejection against the real balance.
    constexpr std::uint8_t kVanguard = mn::kClassVanguard;  // 1
    constexpr std::uint16_t kLevel = 5;
    constexpr mi::Copper kStart = 200;

    std::uint64_t char_id = 0;
    mn::PlaceholderNpcStore store;
    const mn::NpcDef& trainer = *store.find(mn::kNpcTrainer);
    mn::LearnedAbilitySet learned;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);

        db.execute("DELETE FROM `character` WHERE account_id = ?", {sid(account_id)});
        db::Result cr = db.execute(
            "INSERT INTO `character` (account_id, name, race, class, level, money, "
            "map_id, pos_x, pos_y, pos_z) VALUES (?, ?, 1, ?, ?, ?, 0, 0, 0, 0)",
            {sid(account_id), db::Param{cname}, sid(kVanguard), sid(kLevel), sid(kStart)});
        char_id = cr.last_insert_id;
        check("test character minted", char_id > 0);
        check("starts with 200 copper", mi::get_money(db, char_id) == kStart);

        // ---- valid learn: debits the strike's 50 copper ----------------------
        {
            mn::LearnResult r = mn::learn_ability(db, char_id, trainer, mn::kTrainedStrike,
                                                  kVanguard, kLevel, learned);
            check("valid strike learn -> kOk", r.status == mn::TrainStatus::kOk);
            check("result cost is 50", r.cost == 50);
            check("result new_balance is 150", r.new_balance == 150);
            check("character.money debited to 150",
                  mi::get_money(db, char_id) == 150);
            check("strike recorded as learned", learned.knows(mn::kTrainedStrike));
        }

        // ---- rejection: already known -> money untouched ---------------------
        {
            mn::LearnResult r = mn::learn_ability(db, char_id, trainer, mn::kTrainedStrike,
                                                  kVanguard, kLevel, learned);
            check("re-learn strike -> kAlreadyKnown",
                  r.status == mn::TrainStatus::kAlreadyKnown);
            check("already-known: money unchanged at 150",
                  mi::get_money(db, char_id) == 150);
            check("already-known: new_balance echoes 150", r.new_balance == 150);
        }

        // ---- rejection: wrong class -> money untouched -----------------------
        // A Runcaller (class 2) cannot learn the Vanguard-only strike. (The strike
        // is already known for THIS character, but plan order checks class before
        // funds and we pass known=false via a fresh learned set to isolate the
        // class gate; simplest is a different ability the character can't afford —
        // instead assert wrong-class directly with a fresh set.)
        {
            mn::LearnedAbilitySet fresh;
            mn::LearnResult r = mn::learn_ability(db, char_id, trainer, mn::kTrainedStrike,
                                                  /*class=*/2, kLevel, fresh);
            check("wrong-class strike -> kWrongClass",
                  r.status == mn::TrainStatus::kWrongClass);
            check("wrong-class: money unchanged at 150",
                  mi::get_money(db, char_id) == 150);
        }

        // ---- rejection: too-low level -> money untouched ---------------------
        {
            mn::LearnedAbilitySet fresh;
            mn::LearnResult r = mn::learn_ability(db, char_id, trainer, mn::kTrainedHeal,
                                                  kVanguard, /*level=*/1, fresh);
            check("too-low-level heal -> kLevelTooLow",
                  r.status == mn::TrainStatus::kLevelTooLow);
            check("too-low-level: money unchanged at 150",
                  mi::get_money(db, char_id) == 150);
        }

        // ---- rejection: insufficient funds -> money untouched ----------------
        // The heal costs 120 but the balance is 150 — affordable. Drop the balance
        // below 120 first (spend 50 more via a second valid strike? already known).
        // Instead debit directly to set up the unaffordable case: subtract 40 -> 110.
        mi::subtract_money(db, char_id, 40);
        check("balance set to 110 for the funds test", mi::get_money(db, char_id) == 110);
        {
            mn::LearnedAbilitySet fresh;
            mn::LearnResult r = mn::learn_ability(db, char_id, trainer, mn::kTrainedHeal,
                                                  kVanguard, kLevel, fresh);
            check("unaffordable heal -> kInsufficientFunds",
                  r.status == mn::TrainStatus::kInsufficientFunds);
            check("insufficient-funds: money unchanged at 110",
                  mi::get_money(db, char_id) == 110);
        }

        // ---- second valid learn: top up + debit the heal ---------------------
        mi::add_money(db, char_id, 100);  // 110 -> 210
        {
            mn::LearnResult r = mn::learn_ability(db, char_id, trainer, mn::kTrainedHeal,
                                                  kVanguard, kLevel, learned);
            check("valid heal learn -> kOk", r.status == mn::TrainStatus::kOk);
            check("heal debited: 210 -> 90", r.new_balance == 90 &&
                                                 mi::get_money(db, char_id) == 90);
            check("both abilities learned", learned.knows(mn::kTrainedStrike) &&
                                                learned.knows(mn::kTrainedHeal) &&
                                                learned.size() == 2);
        }

    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    // Cleanup: delete the test character row (best-effort).
    try {
        db::Connection db(p);
        if (char_id > 0) db.execute("DELETE FROM `character` WHERE id = ?", {sid(char_id)});
    } catch (...) {
        // best-effort cleanup
    }

    std::printf(g_fail == 0 ? "\nALL NPC DB TESTS PASSED\n"
                            : "\n%d NPC DB TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
