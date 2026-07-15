// SPDX-License-Identifier: Apache-2.0
//
// worldd — DB-LOADED area-trigger POI → explore-objective crediting integration
// test (issue #398, epic #20). Proves the last link that lets explore objectives
// complete against AUTHORED content end-to-end: the world-DB `area` (POI) rows load
// into discovery TriggerVolumes carrying the real `poi`, and a player crossing such a
// DB-loaded volume credits the matching explore quest objective — NOT a synthetic
// placeholder volume.
//
// WHAT IT DOES (throwaway MariaDB + real mcc emit-sql, modeled on the #394
// worldd-itm1-chain-load test):
//   1. runs `mcc emit-sql content/core` -> world DB DML;
//   2. loads the REAL world DDL (schema/sql/world/*.sql) + the emitted DML into a
//      throwaway database it owns (created + dropped here);
//   3. constructs the DB-backed content via worldd::load_world_content() — which now
//      also loads the `area` POI rows into content.area_triggers (#398);
//   4. asserts:
//        (a) the DB-loaded volume set is non-empty and one volume per `area` row;
//        (b) an AUTHORED explore quest ('Warden's Watch') references a POI that has a
//            matching DB-loaded discovery volume — (area_id, poi) == (zone_id, poi);
//        (c) driving AreaTriggerSet::evaluate() with a player position at that POI's
//            centre produces an ENTER TriggerEvent carrying the authored (zone_id,
//            poi) with discovered_now == true;
//        (d) feeding that crossing's (area_id, poi) into a QuestLog holding the REAL
//            (authored) explore objective completes it — i.e. the join runs on the
//            authored `area.poi`, delivered by the DB-loaded volume.
//
// TOOL- + ENV-GUARDED (parity with the other worldd DB tests): reads
// MERIDIAN_WORLDDB_* (falling back to MERIDIAN_DB_*), needs a mariadb/mysql client on
// PATH, and an `mcc` binary. SKIPS (exit 0) when any is absent — inert in the plain
// server ctest, real only under test.sh --db / the DB CI job. Creates + drops the
// throwaway database it owns.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + the #390
// seams + the area_triggers / quest_log headers; no GPL/AGPL/CMaNGOS/TrinityCore/
// leaked source consulted).

#include "area_triggers.h"
#include "db_content_store.h"
#include "quest_def.h"
#include "quest_log.h"

#include "meridian/db/connection.h"

#include <algorithm>
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
#ifdef ITM1_MCC_BIN
    candidates.emplace_back(ITM1_MCC_BIN);
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

// The authored pure-explore quest whose single objective this test credits. Stable
// by NAME (numeric IF-9 ids shift with unrelated content; names are authored identity).
constexpr const char* kExploreQuestName = "Warden's Watch";

// A single-quest store over one QuestDef (a de-gated COPY of the authored explore
// quest), so the crediting path can accept it without satisfying the real chain's
// prerequisites. The explore objective it carries is the AUTHORED one (its zone_id +
// poi come straight from the DB), so the join under test runs on real content.
class OneQuestStore final : public worldd::QuestStore {
public:
    explicit OneQuestStore(worldd::QuestDef q) : def_(std::move(q)) {}
    const worldd::QuestDef* find(worldd::QuestId id) const override {
        return id == def_.id ? &def_ : nullptr;
    }
    std::vector<worldd::QuestId> ids() const override { return {def_.id}; }

private:
    worldd::QuestDef def_;
};

}  // namespace

int main() {
    std::printf("worldd DB-loaded area-trigger POI -> explore crediting test (#398)\n");

    // --- Guards: DB env, client, mcc. SKIP (inert) if any is missing. ----------
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

    fs::path scratch = fs::temp_directory_path() / "explore_trigger_db_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    // --- 1. mcc emit-sql content/core -> world.sql. ----------------------------
    const fs::path world_sql = scratch / "world.sql";
    {
        // --pack core: emit a SINGLE-pack world DB (one pack per realm, design §4).
        // Without it, the merged content/ tree (core + the chibi races pack) emits
        // both packs' per-pack roster_id 1-N rows into one DB and the `race`/class
        // PRIMARY key collides on load (#798).
        std::string cmd = "\"" + mcc + "\" emit-sql \"" + std::string(ITM1_CONTENT_DIR) +
                          "\" --pack core --out \"" + world_sql.string() + "\" >" +
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
        for (const auto& e : fs::directory_iterator(ITM1_WORLD_DDL_DIR)) {
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

    const std::string dbname = "meridian_explore_trigger_test";
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

    // --- 3. Load DDL, then the emitted content DML. ----------------------------
    check("world DDL loads without error", run_sql_file(client, flags, dbname, ddl_sql) == 0);
    check("mcc-emitted world.sql loads without error",
          run_sql_file(client, flags, dbname, world_sql) == 0);

    try {
        db::Connection conn(conn_params(dbname));

        // --- 4. Load the #390/#398 DB-backed content (the boot load path). -----
        worldd::WorldContent content = worldd::load_world_content(conn);

        // (a) The area POI rows loaded into discovery volumes, one per `area` row.
        db::Result area_count = conn.execute("SELECT COUNT(*) FROM area");
        const std::uint64_t db_area_rows =
            (!area_count.rows.empty() && area_count.rows[0][0])
                ? std::strtoull(area_count.rows[0][0]->c_str(), nullptr, 10)
                : 0;
        check("world DB has authored area POI rows", db_area_rows > 0);
        check("a discovery volume is loaded per area row",
              content.area_triggers.size() == db_area_rows && !content.area_triggers.empty());
        {
            bool all_discovery = !content.area_triggers.empty();
            bool all_have_poi = !content.area_triggers.empty();
            for (const worldd::TriggerVolume& v : content.area_triggers) {
                if (v.kind != worldd::TriggerKind::kDiscovery) all_discovery = false;
                if (v.poi.empty()) all_have_poi = false;
            }
            check("every DB-loaded area volume is a discovery volume", all_discovery);
            check("every DB-loaded area volume carries a non-empty poi", all_have_poi);
        }

        // (b) Locate the authored pure-explore quest + its explore objective. Its
        //     (zone_id, poi) is the authored join key.
        const worldd::QuestDef* explore_quest = nullptr;
        for (worldd::QuestId qid : content.quests->ids()) {
            const worldd::QuestDef* q = content.quests->find(qid);
            if (q && q->name == kExploreQuestName) { explore_quest = q; break; }
        }
        check(std::string(std::string("authored explore quest loaded: ") + kExploreQuestName).c_str(),
              explore_quest != nullptr);
        if (!explore_quest) throw db::DbError(0, "explore quest not found");

        const worldd::QuestObjective* explore_obj = nullptr;
        for (const worldd::QuestObjective& o : explore_quest->objectives) {
            if (o.type == worldd::ObjectiveType::kExplore) { explore_obj = &o; break; }
        }
        check("authored quest has an explore objective", explore_obj != nullptr);
        if (!explore_obj) throw db::DbError(0, "explore objective not found");
        const std::uint32_t want_zone = explore_obj->zone_id;
        const std::string   want_poi = explore_obj->poi;
        check("explore objective carries a zone + poi join key",
              want_zone != 0 && !want_poi.empty());

        // A matching DB-loaded discovery volume exists for that authored (zone, poi).
        const worldd::TriggerVolume* target = nullptr;
        for (const worldd::TriggerVolume& v : content.area_triggers) {
            if (v.area_id == want_zone && v.poi == want_poi) { target = &v; break; }
        }
        check("a DB-loaded discovery volume matches the authored explore (zone, poi)",
              target != nullptr);
        if (!target) throw db::DbError(0, "no matching area volume");

        // (c) Crossing the DB-loaded volume fires an ENTER carrying the authored poi.
        worldd::AreaTriggerSet triggers;
        triggers.load(content.area_triggers);  // the authored set (copy — kept for asserts)
        const worldd::AoiId guid = 0x398ULL;

        // Establish occupancy OUTSIDE every volume first, so the move INTO the POI is a
        // real boundary crossing (not a first-touch spawn inside).
        worldd::Position far_away;
        far_away.x = 1.0e6f; far_away.y = 1.0e6f; far_away.z = 0.0f;
        std::vector<worldd::TriggerEvent> none = triggers.evaluate(guid, far_away);
        check("no crossing far from every POI volume", none.empty());

        // Walk to the POI centre (inside the box: [cx-r, cx+r] x [cy-r, cy+r]).
        worldd::Position at_poi;
        at_poi.x = (target->min_x + target->max_x) * 0.5f;
        at_poi.y = (target->min_y + target->max_y) * 0.5f;
        at_poi.z = 0.0f;
        std::vector<worldd::TriggerEvent> crossed = triggers.evaluate(guid, at_poi);

        const worldd::TriggerEvent* enter = nullptr;
        for (const worldd::TriggerEvent& e : crossed) {
            if (e.entered && e.poi == want_poi && e.area_id == want_zone) { enter = &e; break; }
        }
        check("crossing the DB-loaded POI volume fires an ENTER for the authored poi",
              enter != nullptr);
        if (enter) {
            check("the crossing is a first-time discovery (discovered_now)",
                  enter->discovered_now);
        }

        // (d) The crossing's authored (zone, poi) completes the AUTHORED explore
        //     objective. Use a de-gated copy of the real quest (prereqs cleared,
        //     required_level 1) so accept() succeeds standalone — the objective it
        //     carries is the authored one, so the join runs on real `area.poi`.
        worldd::QuestDef degated = *explore_quest;
        degated.prerequisites.clear();
        degated.required_level = 1;
        static OneQuestStore one_store(degated);  // static: outlives the QuestLog below
        worldd::QuestLog log(one_store);
        check("accept the (de-gated) authored explore quest",
              log.accept(degated.id, /*player_level=*/5) == worldd::AcceptStatus::kOk);
        check("explore objective not yet complete before the crossing",
              !log.is_complete(degated.id));

        const bool advanced =
            enter ? log.on_explore(enter->area_id, enter->poi) : false;
        check("DB-loaded volume crossing advances the explore objective", advanced);
        check("explore objective COMPLETE after crossing the DB-loaded POI volume",
              log.is_complete(degated.id));

        // A wrong-poi crossing must NOT credit it (the join really keys on poi).
        worldd::QuestLog log2(one_store);
        log2.accept(degated.id, 5);
        check("a mismatched poi does NOT complete the explore objective",
              !log2.on_explore(want_zone, want_poi + "_nope") &&
                  !log2.is_complete(degated.id));

    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        const fs::path drop = scratch / "drop.sql";
        std::ofstream(drop) << "DROP DATABASE IF EXISTS " << dbname << ";\n";
        run_sql_file(client, flags, "", drop);
        return 1;
    }

    {
        const fs::path drop = scratch / "drop.sql";
        std::ofstream(drop) << "DROP DATABASE IF EXISTS " << dbname << ";\n";
        run_sql_file(client, flags, "", drop);
    }

    if (g_fail == 0) {
        std::printf("PASS: DB-loaded area-trigger POI drives explore crediting\n");
        return 0;
    }
    std::printf("FAIL: %d explore-trigger-DB check(s) failed\n", g_fail);
    return 1;
}
