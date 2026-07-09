// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor DB-backed integration test (ECO-01; issue #370).
//
// Needs a live MariaDB; reads MERIDIAN_DB_* (same var names as the other DB tests)
// and SKIPS (exit 0) when none are set, so it is inert in the DB-free ctest and
// runs for real only in the DB CI job / `scripts/dev/test.sh --db`.
//
// What it proves end-to-end against the characters DB (the REAL server path):
//   1. buy_db      — debits character.money, mints an item_instance, and places it
//                    (character_inventory) — a reload shows the bought item.
//   2. sell_db     — credits character.money, clears the placement + destroys the
//                    instance, and pushes the sold stack onto the buyback queue.
//   3. buyback_db  — re-debits character.money, re-mints + re-places the item.
//   4. insufficient funds — buy_db with too little copper throws InsufficientFunds
//                    and leaves character.money unchanged (transactional, no debit).
//
// Self-contained like item_store_it: CREATE ... IF NOT EXISTS the three tables in
// FK order, operate on its own randomised character, and clean up its rows.
//
// Clean-room, original code (CONTRIBUTING.md).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "buyback.h"
#include "currency.h"
#include "inventory.h"
#include "item_store.h"
#include "item_template.h"
#include "meridian/db/connection.h"
#include "vendor.h"
#include "vendor_catalog.h"

using namespace meridian;

namespace {

int g_fail = 0;
const char* env(const char* k) { return std::getenv(k); }

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

template <typename E, typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

constexpr std::uint32_t B = items::kPlaceholderIdBase;
constexpr std::uint32_t kSword = B + 1;    // buy 100, sell 25
constexpr std::uint32_t kVendor = vendor::kPlaceholderGeneralVendor;

db::Param sid(std::uint64_t v) { return db::Param{std::to_string(v)}; }

// The three characters-DB tables this test touches, in FK order. Columns mirror
// server/db/characters/migrations/0001_init_characters.up.sql. CREATE ... IF NOT
// EXISTS is a no-op when the real migration already loaded them.
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

constexpr const char* kItemInstanceDdl =
    "CREATE TABLE IF NOT EXISTS item_instance ("
    "  item_guid BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  item_template_id INT UNSIGNED NOT NULL,"
    "  stack INT UNSIGNED NOT NULL DEFAULT 1,"
    "  durability INT UNSIGNED NOT NULL DEFAULT 0,"
    "  suffix_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "  creator BIGINT UNSIGNED NULL,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (item_guid),"
    "  KEY idx_item_instance_template (item_template_id),"
    "  KEY idx_item_instance_creator (creator),"
    "  CONSTRAINT fk_item_instance_creator FOREIGN KEY (creator)"
    "    REFERENCES `character` (id) ON DELETE SET NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

constexpr const char* kInventoryDdl =
    "CREATE TABLE IF NOT EXISTS character_inventory ("
    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  char_id BIGINT UNSIGNED NOT NULL,"
    "  bag TINYINT UNSIGNED NOT NULL,"
    "  slot SMALLINT UNSIGNED NOT NULL,"
    "  item_guid BIGINT UNSIGNED NOT NULL,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_inventory_slot (char_id, bag, slot),"
    "  UNIQUE KEY uq_character_inventory_item (item_guid),"
    "  KEY idx_character_inventory_char (char_id),"
    "  CONSTRAINT fk_character_inventory_char FOREIGN KEY (char_id)"
    "    REFERENCES `character` (id) ON DELETE CASCADE,"
    "  CONSTRAINT fk_character_inventory_item FOREIGN KEY (item_guid)"
    "    REFERENCES item_instance (item_guid) ON DELETE CASCADE"
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
    const std::uint64_t account_id = 4'900'000'000ULL + static_cast<unsigned>(salt);
    const std::string cname = "Vnd_" + std::to_string(salt);
    std::uint64_t char_id = 0;
    std::vector<std::uint64_t> minted;  // instance guids to clean up

    items::PlaceholderTemplateStore tmpl;
    vendor::PlaceholderVendorCatalog cat;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        db.execute(kItemInstanceDdl);
        db.execute(kInventoryDdl);

        // Fresh test character seeded with 500 copper.
        db.execute("DELETE FROM `character` WHERE account_id = ?", {sid(account_id)});
        db::Result cr = db.execute(
            "INSERT INTO `character` (account_id, name, race, class, money, map_id, "
            "pos_x, pos_y, pos_z) VALUES (?, ?, 1, 1, 500, 0, 0, 0, 0)",
            {sid(account_id), db::Param{cname}});
        char_id = cr.last_insert_id;
        check("test character minted with 500 copper",
              char_id > 0 && items::get_money(db, char_id) == 500);

        vendor::BuybackQueue buyback;

        // ---- buy: debit + mint + place -------------------------------------
        vendor::BuyDbResult bought =
            vendor::buy_db(db, char_id, cat, tmpl, kVendor, kSword, 1);
        minted.push_back(bought.item_guid);
        check("buy debited the sword price (100)", bought.total_price == 100);
        check("balance is 400 after buy", items::get_money(db, char_id) == 400);
        check("buy minted a server item_guid", bought.item_guid > 0);
        {
            items::Inventory inv = items::load_inventory(db, char_id, tmpl);
            const items::ItemInstance* it = inv.backpack_at(bought.backpack_slot);
            check("bought sword reloads from the characters DB",
                  it && it->template_id == kSword && it->item_guid == bought.item_guid);
        }

        // ---- sell: credit + clear + destroy + buyback ----------------------
        vendor::SellDbResult sold =
            vendor::sell_db(db, char_id, buyback, tmpl, bought.backpack_slot, 1);
        check("sell credited the sell price (25)", sold.total_credit == 25);
        check("balance is 425 after sell", items::get_money(db, char_id) == 425);
        check("sold stack pushed onto buyback", buyback.size() == 1);
        {
            db::Result gone = db.execute(
                "SELECT item_guid FROM item_instance WHERE item_guid = ?",
                {sid(bought.item_guid)});
            check("sold instance row destroyed", gone.rows.empty());
            items::Inventory inv = items::load_inventory(db, char_id, tmpl);
            check("backpack empty after sell", inv.backpack_used() == 0);
        }

        // ---- buyback: re-debit + re-mint + re-place ------------------------
        vendor::BuybackDbResult back = vendor::buyback_db(
            db, char_id, buyback, tmpl, static_cast<std::uint16_t>(sold.buyback_slot));
        minted.push_back(back.item_guid);
        check("buyback re-debited the sale price (25)", back.price == 25);
        check("balance is 400 after buyback", items::get_money(db, char_id) == 400);
        check("buyback queue emptied", buyback.empty());
        {
            items::Inventory inv = items::load_inventory(db, char_id, tmpl);
            const items::ItemInstance* it = inv.backpack_at(back.backpack_slot);
            check("bought-back sword restored in the characters DB",
                  it && it->template_id == kSword && it->item_guid == back.item_guid);
        }

        // ---- insufficient funds leaves money untouched ---------------------
        // Drain to 10 copper, then attempt a 100-copper sword buy.
        items::subtract_money(db, char_id, items::get_money(db, char_id) - 10);
        check("balance drained to 10", items::get_money(db, char_id) == 10);
        check("unaffordable buy throws InsufficientFunds",
              throws<items::InsufficientFunds>(
                  [&] { vendor::buy_db(db, char_id, cat, tmpl, kVendor, kSword, 1); }));
        check("balance unchanged after refused buy (no debit)",
              items::get_money(db, char_id) == 10);

    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    // Cleanup: destroy any minted instances (removes placements via CASCADE), then
    // delete the test character.
    try {
        db::Connection db(p);
        for (std::uint64_t guid : minted) {
            db.execute("DELETE FROM item_instance WHERE item_guid = ?", {sid(guid)});
        }
        if (char_id > 0) {
            db.execute("DELETE FROM `character` WHERE id = ?", {sid(char_id)});
        }
    } catch (...) {
        // best-effort cleanup
    }

    std::printf(g_fail == 0 ? "\nALL VENDOR DB TESTS PASSED\n"
                            : "\n%d VENDOR DB TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
