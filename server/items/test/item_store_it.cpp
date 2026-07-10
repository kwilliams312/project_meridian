// SPDX-License-Identifier: Apache-2.0
//
// meridian-items DB-backed integration test (ITM-01 + ECO-01; issue #366).
//
// Needs a live MariaDB; reads MERIDIAN_DB_* (same var names as the other DB
// tests) and SKIPS (exit 0) when none are set, so it is inert in the DB-free
// ctest and runs for real only in the DB CI job / `scripts/dev/test.sh --db`.
//
// What it proves end-to-end against the characters DB:
//   1. mint_instance mints a server-assigned item_guid (item_instance).
//   2. place_item + load_inventory round-trip: minted instances placed in
//      backpack + equipment slots reload into the exact same slots with fields
//      intact (the inventory.h slot mapping <-> character_inventory).
//   3. set_instance_stack / move_placement / clear_placement / destroy_instance
//      mutate durable state as expected on reload.
//   4. currency: get/add/subtract_money persist to character.money; an
//      unaffordable subtract is refused (InsufficientFunds) and leaves the
//      balance unchanged (transactional, no underflow).
//
// Self-contained like characters_test: CREATE ... IF NOT EXISTS the three tables
// (character, item_instance, character_inventory) in FK order, operate on its own
// randomised character, and clean up its rows. It never drops a table.
//
// Clean-room, original code (CONTRIBUTING.md).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "currency.h"
#include "inventory.h"
#include "item_store.h"
#include "item_template.h"
#include "meridian/db/connection.h"

using namespace meridian;
using namespace meridian::items;

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

constexpr std::uint32_t kSword = kPlaceholderIdBase + 1;    // main_hand weapon
constexpr std::uint32_t kBuckler = kPlaceholderIdBase + 2;  // off_hand armor
constexpr std::uint32_t kPotion = kPlaceholderIdBase + 7;   // consumable, stackable

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
    const std::uint64_t account_id = 4'700'000'000ULL + static_cast<unsigned>(salt);
    const std::string cname = "Itm_" + std::to_string(salt);
    std::uint64_t char_id = 0;
    std::vector<std::uint64_t> minted;  // instance guids to clean up

    PlaceholderTemplateStore store;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        db.execute(kItemInstanceDdl);
        db.execute(kInventoryDdl);

        // Fresh test character (money defaults to 0).
        db.execute("DELETE FROM `character` WHERE account_id = ?", {sid(account_id)});
        db::Result cr = db.execute(
            "INSERT INTO `character` (account_id, name, race, class, map_id, "
            "pos_x, pos_y, pos_z) VALUES (?, ?, 1, 1, 0, 0, 0, 0)",
            {sid(account_id), db::Param{cname}});
        char_id = cr.last_insert_id;
        check("test character minted", char_id > 0);

        // ---- currency -------------------------------------------------------
        check("new character has 0 copper", get_money(db, char_id) == 0);
        check("add 500 copper -> 500", add_money(db, char_id, 500) == 500);
        check("persisted balance is 500", get_money(db, char_id) == 500);
        check("subtract 200 -> 300", subtract_money(db, char_id, 200) == 300);

        check("unaffordable subtract throws InsufficientFunds",
              throws<InsufficientFunds>([&] { subtract_money(db, char_id, 400); }));
        check("balance unchanged after refused subtract (no underflow)",
              get_money(db, char_id) == 300);
        check("negative add throws NegativeAmount",
              throws<NegativeAmount>([&] { add_money(db, char_id, -1); }));
        check("get_money on a missing character throws CharacterNotFound",
              throws<CharacterNotFound>([&] { get_money(db, 0); }));

        // ---- mint + place + load round-trip --------------------------------
        ItemInstance sword = mint_instance(db, kSword, 1, /*creator=*/char_id);
        ItemInstance potion = mint_instance(db, kPotion, 5);
        ItemInstance buckler = mint_instance(db, kBuckler, 1);
        minted = {sword.item_guid, potion.item_guid, buckler.item_guid};
        check("mint assigns server item_guids",
              sword.item_guid > 0 && potion.item_guid > 0 && buckler.item_guid > 0);

        // sword -> backpack slot 0, potion -> backpack slot 1, buckler -> off hand.
        place_item(db, char_id, 0, backpack_placement_slot(0), sword.item_guid);
        place_item(db, char_id, 0, backpack_placement_slot(1), potion.item_guid);
        place_item(db, char_id, 0, equip_placement_slot(EquipSlot::kOffHand),
                   buckler.item_guid);

        {
            Inventory inv = load_inventory(db, char_id, store);
            const ItemInstance* b0 = inv.backpack_at(0);
            const ItemInstance* b1 = inv.backpack_at(1);
            const ItemInstance* off = inv.equipped_at(EquipSlot::kOffHand);
            check("loaded sword in backpack slot 0",
                  b0 && b0->template_id == kSword && b0->item_guid == sword.item_guid);
            check("loaded creator round-trips", b0 && b0->creator == char_id);
            check("loaded potion stack of 5 in backpack slot 1",
                  b1 && b1->template_id == kPotion && b1->stack == 5);
            check("loaded buckler in the off-hand equip slot",
                  off && off->template_id == kBuckler);
            check("nothing loaded into the main hand",
                  inv.equipped_at(EquipSlot::kMainHand) == nullptr);
        }

        // ---- mutate durable state ------------------------------------------
        check("set_instance_stack updates the potion",
              set_instance_stack(db, potion.item_guid, 3));
        check("move_placement relocates the sword to backpack slot 2",
              move_placement(db, char_id, 0, backpack_placement_slot(2),
                             sword.item_guid));
        {
            Inventory inv = load_inventory(db, char_id, store);
            check("potion stack reloads as 3",
                  inv.backpack_at(1) && inv.backpack_at(1)->stack == 3);
            check("sword reloads in backpack slot 2 (moved)",
                  inv.backpack_at(2) && inv.backpack_at(2)->template_id == kSword);
            check("sword's old slot 0 is now empty", inv.backpack_at(0) == nullptr);
        }

        check("clear_placement removes the potion placement",
              clear_placement(db, char_id, potion.item_guid));
        {
            Inventory inv = load_inventory(db, char_id, store);
            check("potion no longer placed after clear", inv.backpack_at(1) == nullptr);
        }
        // The instance row survives a cleared placement.
        db::Result still = db.execute(
            "SELECT stack FROM item_instance WHERE item_guid = ?",
            {sid(potion.item_guid)});
        check("cleared item's instance row still exists", still.rows.size() == 1);

        check("destroy_instance deletes the potion row",
              destroy_instance(db, potion.item_guid));
        db::Result gone = db.execute(
            "SELECT item_guid FROM item_instance WHERE item_guid = ?",
            {sid(potion.item_guid)});
        check("destroyed instance row is gone", gone.rows.empty());

    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    // Cleanup: destroy any minted instances (removes their placements via CASCADE),
    // then delete the test character (CASCADE clears remaining placements).
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

    std::printf(g_fail == 0 ? "\nALL ITEM DB TESTS PASSED\n"
                            : "\n%d ITEM DB TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
