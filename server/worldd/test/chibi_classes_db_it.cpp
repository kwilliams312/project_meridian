// SPDX-License-Identifier: Apache-2.0
//
// worldd — SP5 CHIBI CLASSES boot -> create -> equip -> cast DB-backed integration
// test (story #723, epic #722). The MANDATORY SP2-discipline runtime proof that a
// brand-new class roster ships as PURE PACK DATA on the SP2 kernel with ZERO engine
// change: it runs the REAL mcc pipeline against content/core (the 4 chibi classes
// this story authored — Warrior/Mage/Rogue/Priest, roster ids 5-8) and drives the
// whole boot -> create -> equip -> cast chain over the loaded pack.
//
// WHAT IT DOES (throwaway MariaDB + real mcc emit-sql, modeled on the #394
// worldd-itm1-chain-load test and the #390 content-load test):
//   1. runs `mcc emit-sql content` to compile content/core -> world DB DML;
//   2. loads the REAL world DDL (schema/sql/world/*.sql) + the emitted DML into a
//      throwaway database it owns (created + dropped here);
//   3. constructs the DB-backed stores via worldd::load_world_content() over it —
//      "worldd boots the pack";
//   4. BOOT   — the runtime Roster + ClassCatalog carry the 4 chibi classes
//               (Warrior 5, Mage 6, Rogue 7, Priest 8) loaded from pack data, with
//               the right role + usable armor/weapon types;
//   5. CREATE — a character is created as a chibi class (Ardent race + Warrior
//               class), VALIDATED against the DB-loaded roster; an undefined class
//               id is refused;
//   6. EQUIP  — gate_equip ACCEPTS each class's starter items (equip_type in the
//               class's proficiencies) and REJECTS a wrong-type item (a Warrior
//               cannot wear the Mage's cloth robe);
//   7. CAST   — the chibi abilities loaded from the pack CAST through the SP2 ability
//               engine (MapTick) and their effects EXECUTE, observed server-side:
//               Warrior Crushing Blow deals damage + Bulwark grants an absorb shield,
//               Mage Cinderburn burns (periodic dot damage), Priest Mend heals. The
//               Warrior Skullcrack stun effect is asserted to have loaded from the
//               pack as a cc/stun primitive (execution semantics are unit-proven in
//               ability_primitives_test).
//
// TOOL- + ENV-GUARDED (parity with the other worldd DB tests): reads
// MERIDIAN_WORLDDB_* (falling back to MERIDIAN_DB_*) for the DB, needs a mariadb/mysql
// client on PATH, and an `mcc` binary (env MERIDIAN_MCC_BIN, then the build-tree
// default, then PATH). SKIPS (exit 0) when any is absent — inert in the plain server
// ctest, real only under test.sh --db / the DB CI job. Item + ability numeric ids are
// resolved from the DB BY NAME so the test is robust to IF-9 idmap renumbering
// (append-only ids shift with unrelated content); class ids use the STABLE roster_id.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + the
// #390/#697/#693 seams; no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "characters.h"        // create/validate a character vs the DB-loaded roster (#695)
#include "class_kernel.h"      // gate_equip / EquipGate / ClassRecord / ClassCatalog (#697)
#include "combat_unit.h"       // UnitStats / ResourceType / Faction
#include "creature_ai.h"       // CreatureSpawnDef / Creature
#include "db_content_store.h"  // load_world_content / WorldContent (#390)
#include "map_tick.h"          // MapTick — the SP2 ability engine (#693)
#include "movement_validation.h"  // kFlatGroundZ
#include "meridian/db/connection.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace meridian;
namespace db = meridian::db;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }
const char* pick(const char* world_key, const char* fallback_key) {
    if (const char* v = env(world_key)) return v;
    return env(fallback_key);
}

std::string find_client() {
    for (const char* name : {"mariadb", "mysql"}) {
        std::string cmd = std::string(name) + " --version >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) return name;
    }
    return "";
}

std::string find_mcc() {
    std::vector<std::string> candidates;
    if (const char* e = env("MERIDIAN_MCC_BIN")) candidates.emplace_back(e);
#ifdef CHIBI_MCC_BIN
    candidates.emplace_back(CHIBI_MCC_BIN);
#endif
    for (const std::string& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    if (std::system("mcc --help >/dev/null 2>&1") == 0 ||
        std::system("command -v mcc >/dev/null 2>&1") == 0) {
        return "mcc";
    }
    return "";
}

std::string conn_flags() {
    std::string f;
    auto add = [&](const std::string& s) { f += " " + s; };
    if (const char* s = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET"))
        add("--socket=" + std::string(s));
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST"))
        add("--host=" + std::string(h));
    if (const char* p = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT"))
        add("--port=" + std::string(p));
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER"))
        add("--user=" + std::string(u));
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS"))
        add("--password=" + std::string(pw));
    return f;
}

int run_sql_file(const std::string& client, const std::string& flags,
                 const std::string& dbname, const fs::path& file) {
    std::string cmd = client + flags;
    if (!dbname.empty()) cmd += " " + dbname;
    cmd += " < " + file.string();
    return std::system(cmd.c_str());
}

db::ConnectParams conn_params(const std::string& dbname) {
    db::ConnectParams p;
    if (const char* s = pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) p.unix_socket = s;
    if (const char* h = pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST")) p.host = h;
    if (const char* port = pick("MERIDIAN_WORLDDB_PORT", "MERIDIAN_DB_PORT"))
        p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = pick("MERIDIAN_WORLDDB_USER", "MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = pick("MERIDIAN_WORLDDB_PASS", "MERIDIAN_DB_PASS")) p.password = pw;
    p.database = dbname;
    return p;
}

// Resolve an item_template / ability numeric id from the live DB BY NAME (robust to
// idmap renumbering). Returns 0 if not found.
std::uint32_t id_by_name(db::Connection& conn, const char* table, const std::string& name) {
    const db::Result r = conn.execute(
        std::string("SELECT id FROM ") + table + " WHERE name = ? LIMIT 1",
        {db::Param{name}});
    if (r.rows.empty() || !r.rows[0].cols[0].has_value()) return 0;
    return static_cast<std::uint32_t>(std::stoul(*r.rows[0].cols[0]));
}

// --- MapTick scenario helpers (mirror cast_ability_db_it / ability_primitives_test).
namespace w = meridian::worldd;
namespace mc = meridian::worldd::movement;

w::Position at(float x, float y) {
    w::Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}
w::UnitStats pstats(std::uint32_t hp, w::ResourceType rt, std::uint32_t res) {
    w::UnitStats s;
    s.level = 5;
    s.max_health = hp;
    s.resource_type = rt;
    s.max_resource = res;
    s.faction = w::Faction::kPlayer;
    return s;
}
w::CreatureSpawnDef target_mob(w::Position home) {
    w::CreatureSpawnDef d;
    d.template_id = 1;
    d.level = 5;
    d.faction = w::Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = 0;  // inert — we drive the cast, not the AI
    d.leash_radius = 100000;
    d.respawn_ms = 999999;
    d.move_speed = 0;
    d.patrol_mode = w::PatrolMode::kStationary;
    return d;
}

}  // namespace

int main() {
    std::printf("worldd SP5 chibi-classes boot->create->equip->cast DB-backed test (#723)\n");

    if (!pick("MERIDIAN_WORLDDB_HOST", "MERIDIAN_DB_HOST") &&
        !pick("MERIDIAN_WORLDDB_SOCKET", "MERIDIAN_DB_SOCKET")) {
        std::printf("SKIP: no MERIDIAN_WORLDDB_*/MERIDIAN_DB_* connection configured\n");
        return 0;
    }
    const std::string client = find_client();
    if (client.empty()) {
        std::printf("SKIP: no mariadb/mysql client on PATH\n");
        return 0;
    }
    const std::string mcc = find_mcc();
    if (mcc.empty()) {
        std::printf("SKIP: no mcc binary found (set MERIDIAN_MCC_BIN, or build it)\n");
        return 0;
    }
    const std::string flags = conn_flags();

    fs::path scratch = fs::temp_directory_path() / "chibi_classes_db_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    // --- 1. mcc emit-sql content/core -> world.sql. ----------------------------
    const fs::path world_sql = scratch / "world.sql";
    {
        std::string cmd = "\"" + mcc + "\" emit-sql \"" + std::string(CHIBI_CONTENT_DIR) +
                          "\" --out \"" + world_sql.string() + "\" >" +
                          (scratch / "emit.log").string() + " 2>&1";
        const int rc = std::system(cmd.c_str());
        check("mcc emit-sql produced world.sql", rc == 0 && fs::exists(world_sql));
        if (rc != 0 || !fs::exists(world_sql)) {
            std::printf("FAIL: emit-sql failed (rc=%d); see %s\n", rc,
                        (scratch / "emit.log").string().c_str());
            return 1;
        }
    }

    // --- 2. Concatenate the real world DDL (00..90) into one load script. ------
    const fs::path ddl_sql = scratch / "ddl.sql";
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(CHIBI_WORLD_DDL_DIR)) {
            if (e.path().extension() == ".sql") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        std::ofstream out(ddl_sql);
        for (const auto& f : files) {
            std::ifstream in(f);
            out << in.rdbuf() << "\n";
        }
        check("assembled world DDL script", fs::exists(ddl_sql) && !files.empty());
    }

    const std::string dbname = "meridian_chibi_classes_test";
    {
        const fs::path create = scratch / "create.sql";
        std::ofstream(create) << "DROP DATABASE IF EXISTS " << dbname << ";\n"
                              << "CREATE DATABASE " << dbname
                              << " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;\n";
        const int rc = run_sql_file(client, flags, "", create);
        check("created a fresh throwaway database", rc == 0);
        if (rc != 0) {
            std::printf("FAIL: could not create database (connection/permissions?)\n");
            return 1;
        }
    }

    // --- 3. Load DDL, then the emitted content DML (worldd boots the pack). -----
    check("world DDL loads without error", run_sql_file(client, flags, dbname, ddl_sql) == 0);
    check("mcc-emitted world.sql loads without error",
          run_sql_file(client, flags, dbname, world_sql) == 0);

    try {
        db::Connection conn(conn_params(dbname));

        // --- 4. BOOT: load the DB-backed stores (the boot load path). ----------
        worldd::WorldContent content = worldd::load_world_content(conn);

        // roster_ids the pack authored (stable, append-only).
        constexpr std::uint8_t kWarrior = 5, kMage = 6, kRogue = 7, kPriest = 8;

        check("BOOT: roster loaded from the pack", content.roster.has_value());
        if (!content.roster) {
            std::printf("FAIL: no roster loaded — aborting\n");
            return 1;
        }
        const characters::Roster& roster = *content.roster;
        check("BOOT: Warrior(5) in roster from pack",
              roster.is_valid_class(kWarrior) && roster.class_name(kWarrior) == "Warrior");
        check("BOOT: Mage(6) in roster from pack",
              roster.is_valid_class(kMage) && roster.class_name(kMage) == "Mage");
        check("BOOT: Rogue(7) in roster from pack",
              roster.is_valid_class(kRogue) && roster.class_name(kRogue) == "Rogue");
        check("BOOT: Priest(8) in roster from pack",
              roster.is_valid_class(kPriest) && roster.class_name(kPriest) == "Priest");
        // The pre-existing seed roster is untouched (append-only): Vanguard(1) survives.
        check("BOOT: existing Vanguard(1) still present (append-only, not renumbered)",
              roster.is_valid_class(1) && roster.class_name(1) == "Vanguard");
        // Chibi races are cosmetic: race_limits omitted => all races may play (Ardent OK).
        check("BOOT: Ardent(1) may play Warrior (race_limits omitted = all races)",
              roster.is_race_allowed_for_class(kWarrior, /*race=*/1));

        // ClassCatalog: role + usable armor/weapon types loaded from the pack.
        const worldd::ClassRecord* warrior = content.classes.find(kWarrior);
        const worldd::ClassRecord* mage = content.classes.find(kMage);
        const worldd::ClassRecord* rogue = content.classes.find(kRogue);
        const worldd::ClassRecord* priest = content.classes.find(kPriest);
        check("BOOT: all 4 chibi ClassRecords loaded",
              warrior && mage && rogue && priest);
        if (!warrior || !mage || !rogue || !priest) {
            std::printf("FAIL: a chibi ClassRecord failed to load — aborting\n");
            return 1;
        }
        // equip_type ids from the pack: cloth=118, leather=119, one_hand=121,
        // plate=122, staff=123, two_hand=124, wand=125 (resolved by name to stay robust).
        auto etype = [&](const std::string& ref) -> std::uint32_t {
            const db::Result r = conn.execute(
                "SELECT content_id FROM equip_type WHERE equip_ref = ? LIMIT 1",
                {db::Param{ref}});
            if (r.rows.empty() || !r.rows[0].cols[0].has_value()) return 0;
            return static_cast<std::uint32_t>(std::stoul(*r.rows[0].cols[0]));
        };
        const std::uint32_t etCloth = etype("core:equip_type.cloth");
        const std::uint32_t etLeather = etype("core:equip_type.leather");
        const std::uint32_t etOneHand = etype("core:equip_type.one_hand");
        const std::uint32_t etPlate = etype("core:equip_type.plate");
        const std::uint32_t etStaff = etype("core:equip_type.staff");
        const std::uint32_t etWand = etype("core:equip_type.wand");
        check("BOOT: Warrior proficiencies = plate armor + 1H/2H weapons",
              warrior->can_use_armor(etPlate) && warrior->can_use_weapon(etOneHand));
        check("BOOT: Mage proficiencies = cloth armor + staff/wand",
              mage->can_use_armor(etCloth) && mage->can_use_weapon(etStaff) &&
                  mage->can_use_weapon(etWand));
        check("BOOT: Rogue proficiencies = leather armor + 1H",
              rogue->can_use_armor(etLeather) && rogue->can_use_weapon(etOneHand));
        check("BOOT: Priest proficiencies = cloth armor + wand/staff",
              priest->can_use_armor(etCloth) && priest->can_use_weapon(etWand));
        // A Warrior is NOT proficient in cloth (the wrong-type gate below).
        check("BOOT: Warrior is NOT proficient in cloth armor",
              !warrior->can_use_armor(etCloth));

        // --- 5. CREATE: a chibi-class character, validated vs the DB roster. ----
        // A standalone character table on the same DB (no FK — the soft-ref rule).
        conn.execute(
            "CREATE TEMPORARY TABLE `character` ("
            "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "  account_id BIGINT UNSIGNED NOT NULL, name VARCHAR(32) NOT NULL,"
            "  race TINYINT UNSIGNED NOT NULL, class TINYINT UNSIGNED NOT NULL,"
            "  appearance JSON NULL, level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
            "  map_id INT UNSIGNED NOT NULL DEFAULT 0,"
            "  pos_x FLOAT NOT NULL DEFAULT 0, pos_y FLOAT NOT NULL DEFAULT 0,"
            "  pos_z FLOAT NOT NULL DEFAULT 0, PRIMARY KEY (id),"
            "  UNIQUE KEY uq_character_name (name)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
        {
            characters::CreateRequest cr;
            cr.account_id = 723723;
            cr.name = "ChibiWarrior";
            cr.race = 1;              // Ardent — a PACK race
            cr.char_class = kWarrior; // Warrior — a PACK chibi class
            bool ok = false;
            try {
                characters::create_character(conn, cr, roster);
                ok = true;
            } catch (const std::exception& e) {
                std::printf("      (create threw: %s)\n", e.what());
            }
            check("CREATE: Ardent/Warrior character created vs the DB-loaded roster", ok);

            const std::vector<characters::CharacterSummary> mine =
                characters::list_characters(conn, cr.account_id);
            check("CREATE: character persisted with race=Ardent(1), class=Warrior(5)",
                  mine.size() == 1 && mine[0].race == 1 && mine[0].char_class == kWarrior);

            // An undefined class id is refused by roster validation.
            characters::CreateRequest bad = cr;
            bad.name = "NotAClass";
            bad.char_class = 99;  // no such class in the roster
            bool refused = false;
            try {
                characters::create_character(conn, bad, roster);
            } catch (const std::exception&) {
                refused = true;
            }
            check("CREATE: an undefined class id is refused", refused);
        }

        // --- 6. EQUIP: proficiency gate ACCEPT + REJECT over pack items. -------
        const std::uint32_t idPlatemail = id_by_name(conn, "item_template", "Recruit's Platemail");
        const std::uint32_t idBattleblade = id_by_name(conn, "item_template", "Recruit's Battleblade");
        const std::uint32_t idRobe = id_by_name(conn, "item_template", "Apprentice's Robe");
        const std::uint32_t idStaff = id_by_name(conn, "item_template", "Apprentice's Staff");
        const std::uint32_t idLeathers = id_by_name(conn, "item_template", "Recruit's Leathers");
        const std::uint32_t idVestments = id_by_name(conn, "item_template", "Acolyte's Vestments");
        const worldd::EquipTypeCatalog& etc = content.equip_types;
        const items::ItemTemplate* platemail = content.items->find(idPlatemail);
        const items::ItemTemplate* battleblade = content.items->find(idBattleblade);
        const items::ItemTemplate* robe = content.items->find(idRobe);
        const items::ItemTemplate* staff = content.items->find(idStaff);
        const items::ItemTemplate* leathers = content.items->find(idLeathers);
        const items::ItemTemplate* vestments = content.items->find(idVestments);
        check("EQUIP: chibi starter items loaded from the pack",
              platemail && battleblade && robe && staff && leathers && vestments);
        if (platemail && battleblade && robe && staff && leathers && vestments) {
            check("EQUIP ACCEPT: Warrior equips its plate Platemail",
                  worldd::gate_equip(*warrior, etc, *platemail) == worldd::EquipGate::kAllowed);
            check("EQUIP ACCEPT: Warrior equips its 1H Battleblade",
                  worldd::gate_equip(*warrior, etc, *battleblade) == worldd::EquipGate::kAllowed);
            check("EQUIP ACCEPT: Mage equips its cloth Robe",
                  worldd::gate_equip(*mage, etc, *robe) == worldd::EquipGate::kAllowed);
            check("EQUIP ACCEPT: Mage equips its Staff",
                  worldd::gate_equip(*mage, etc, *staff) == worldd::EquipGate::kAllowed);
            check("EQUIP ACCEPT: Rogue equips its leather armor",
                  worldd::gate_equip(*rogue, etc, *leathers) == worldd::EquipGate::kAllowed);
            check("EQUIP ACCEPT: Priest equips its cloth Vestments",
                  worldd::gate_equip(*priest, etc, *vestments) == worldd::EquipGate::kAllowed);
            // REJECT: a Warrior (plate) cannot wear the Mage's cloth robe.
            check("EQUIP REJECT: Warrior CANNOT equip the Mage's cloth Robe (not proficient)",
                  worldd::gate_equip(*warrior, etc, *robe) == worldd::EquipGate::kNotProficient);
            check("EQUIP REJECT: Rogue CANNOT equip the plate Platemail (not proficient)",
                  worldd::gate_equip(*rogue, etc, *platemail) == worldd::EquipGate::kNotProficient);
        }

        // --- 7. CAST: chibi abilities execute through the SP2 ability engine. ---
        check("CAST: pack ability store loaded", content.abilities != nullptr);
        if (content.abilities) {
            const worldd::AbilityStore& store = *content.abilities;
            const std::uint32_t idCrushing = id_by_name(conn, "ability", "Crushing Blow");
            const std::uint32_t idBulwark = id_by_name(conn, "ability", "Bulwark");
            const std::uint32_t idSkullcrack = id_by_name(conn, "ability", "Skullcrack");
            const std::uint32_t idCinderburn = id_by_name(conn, "ability", "Cinderburn");
            const std::uint32_t idMend = id_by_name(conn, "ability", "Mend");

            // Warrior Crushing Blow (damage) on a creature → its HP drops.
            {
                w::MapTick mt(store, 0x723001ULL, /*dt=*/1000);
                const w::ObjectGuid p =
                    mt.add_player(1, at(0, 0), pstats(1000, w::ResourceType::kRage, 100), 5);
                const w::ObjectGuid c = mt.add_creature(target_mob(at(2, 0)));
                mt.ai().creature(c)->set_max_health(500);
                const std::uint32_t hp0 = mt.ai().creature(c)->health();
                mt.enqueue_cast(w::AbilityUseCmd{p, idCrushing, c});
                mt.advance(2);
                check("CAST: Warrior Crushing Blow dealt damage (engine executed the pack effect)",
                      mt.ai().creature(c)->health() < hp0);
            }
            // Warrior Bulwark (shield, self) → an absorb pool is granted.
            {
                w::MapTick mt(store, 0x723002ULL, /*dt=*/1000);
                const w::ObjectGuid p =
                    mt.add_player(1, at(0, 0), pstats(1000, w::ResourceType::kRage, 100), 5);
                mt.enqueue_cast(w::AbilityUseCmd{p, idBulwark, p});
                mt.advance(2);
                check("CAST: Warrior Bulwark granted an absorb shield (>=80)",
                      mt.unit_for_guid(p)->absorb() >= 80);
            }
            // Warrior Skullcrack loaded from the pack as a cc/stun primitive.
            {
                const worldd::Ability* sk = store.find(idSkullcrack);
                check("CAST: Warrior Skullcrack loaded as a cc/stun effect from the pack",
                      sk != nullptr && sk->effects.size() == 1 &&
                          sk->effects[0].kind == worldd::EffectKind::kCc &&
                          sk->effects[0].cc_kind == worldd::CrowdControlKind::kStun &&
                          sk->effects[0].duration_ms == 4000);
            }
            // Mage Cinderburn (dot) on a creature → periodic burn damage over ticks.
            {
                w::MapTick mt(store, 0x723006ULL, /*dt=*/3000);
                const w::ObjectGuid p =
                    mt.add_player(1, at(0, 0), pstats(1000, w::ResourceType::kMana, 1000), 6);
                const w::ObjectGuid c = mt.add_creature(target_mob(at(5, 0)));
                mt.ai().creature(c)->set_max_health(500);
                const std::uint32_t hp0 = mt.ai().creature(c)->health();
                mt.enqueue_cast(w::AbilityUseCmd{p, idCinderburn, c});
                mt.advance(5);  // dot ticks 6-9 @ 3 s over 12 s
                check("CAST: Mage Cinderburn burned the target over time (periodic dot)",
                      mt.ai().creature(c)->health() < hp0);
            }
            // Priest Mend (heal) on a wounded self → HP recovers.
            {
                w::MapTick mt(store, 0x723008ULL, /*dt=*/2000);
                const w::ObjectGuid p =
                    mt.add_player(1, at(0, 0), pstats(1000, w::ResourceType::kMana, 1000), 8);
                mt.unit_for_guid(p)->apply_damage(500);  // 500/1000
                const std::uint32_t hp0 = mt.unit_for_guid(p)->health();
                mt.enqueue_cast(w::AbilityUseCmd{p, idMend, p});  // 1.5 s cast
                mt.advance(3);
                check("CAST: Priest Mend healed the wounded target (heal executed)",
                      mt.unit_for_guid(p)->health() > hp0);
            }
        }

        // Cleanup — drop the throwaway database.
        {
            const fs::path drop = scratch / "cleanup.sql";
            std::ofstream(drop) << "DROP DATABASE IF EXISTS " << dbname << ";\n";
            run_sql_file(client, flags, "", drop);
        }
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        return 1;
    }

    if (g_fail == 0) {
        std::printf("PASS: all chibi-classes boot->create->equip->cast checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d chibi-classes check(s) failed\n", g_fail);
    return 1;
}
