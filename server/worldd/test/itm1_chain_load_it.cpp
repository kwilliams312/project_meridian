// SPDX-License-Identifier: Apache-2.0
//
// worldd — IT-M1 10-QUEST CHAIN content CONSISTENCY + LOAD integration test
// (issue #394, epic #20). The content half of the epic-#20 exit criterion: prove
// the hand-authored 10-quest greybox chain in content/core BUILDS via `mcc
// emit-sql`, LOADS through the #390 DB-backed content stores, and is INTERNALLY
// CONSISTENT — the substrate the follow-up server-side IT-M1 harness will drive.
//
// WHAT IT DOES (throwaway MariaDB + real mcc emit-sql, modeled on the #390
// worldd-content-load-db test and the mcc emit-sql DB test):
//   1. runs `mcc emit-sql content` to compile content/core -> world DB DML;
//   2. loads the REAL world DDL (schema/sql/world/*.sql) + the emitted DML into a
//      throwaway database it owns (created + dropped here);
//   3. constructs the DB-backed stores via worldd::load_world_content() over it;
//   4. asserts the chain is self-consistent:
//        * all 10 chain quests load;
//        * prerequisites form a valid DAG — every prereq resolves, no cycle;
//        * every objective TARGET resolves — killed creature exists, collected /
//          delivered item exists, explore POI exists in the zone's `area` rows;
//        * every reward item + giver / turn-in NPC resolves;
//        * the loot tables actually DROP the items the `collect` objectives need.
//
// TOOL- + ENV-GUARDED (parity with the other worldd DB tests): reads
// MERIDIAN_WORLDDB_* (falling back to MERIDIAN_DB_*) for the DB, needs a
// mariadb/mysql client on PATH, and an `mcc` binary (env MERIDIAN_MCC_BIN, then the
// build-tree default, then PATH). SKIPS (exit 0) when any of those is absent — so it
// is inert in the plain server ctest and runs for real only under test.sh --db /
// the DB CI job. It creates + drops the throwaway database it owns, touching nothing
// else.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR world DDL + the
// #390 seams; no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted).

#include "db_content_store.h"
#include "meridian/db/connection.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Locate a MariaDB/MySQL client binary. Returns "" if none is on PATH.
std::string find_client() {
    for (const char* name : {"mariadb", "mysql"}) {
        std::string cmd = std::string(name) + " --version >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) return name;
    }
    return "";
}

// Locate the mcc binary: env override, then the build-tree default the CMake
// compile-def points at, then PATH. Returns "" if none is runnable.
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
    // PATH fallback.
    if (std::system("mcc --help >/dev/null 2>&1") == 0 ||
        std::system("command -v mcc >/dev/null 2>&1") == 0) {
        return "mcc";
    }
    return "";
}

// Shared client connection flags built from the env (mirrors the mcc DB test).
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

// db::ConnectParams from the same env the client uses, aimed at `dbname`.
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

// The 10 quests that make up the authored IT-M1 chain, by display name (stable —
// numeric IF-9 ids are append-only in the idmap and would shift with unrelated
// content, but names are the authored identity).
const std::vector<std::string> kChainQuestNames = {
    "Culling the Kobolds", "Ears for Evidence",      "Sparks in the Dark",
    "Warden's Watch",      "Emberblight",            "Trail of the Digmasters",
    "The Deep Tally",      "Silence the Firecallers", "Charter of the Hollow",
    "Heart of the Cindermaw",
};

}  // namespace

int main() {
    std::printf("worldd IT-M1 10-quest chain content consistency + load test (#394)\n");

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

    fs::path scratch = fs::temp_directory_path() / "itm1_chain_load_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    // --- 1. mcc emit-sql content/core -> world.sql. ----------------------------
    const fs::path world_sql = scratch / "world.sql";
    {
        std::string cmd = "\"" + mcc + "\" emit-sql \"" + std::string(ITM1_CONTENT_DIR) +
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

    const std::string dbname = "meridian_itm1_chain_test";
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

        // --- 4. Load the #390 DB-backed stores (the boot load path). -----------
        worldd::WorldContent content = worldd::load_world_content(conn);

        // Index every loaded quest by NAME so the chain is identified by its
        // authored identity, and keep the id->name map for DAG messages.
        std::unordered_map<std::string, const worldd::QuestDef*> by_name;
        std::unordered_map<worldd::QuestId, const worldd::QuestDef*> by_id;
        for (worldd::QuestId qid : content.quests->ids()) {
            const worldd::QuestDef* q = content.quests->find(qid);
            if (q) {
                by_name[q->name] = q;
                by_id[qid] = q;
            }
        }

        // ---- (a) All 10 chain quests load. ------------------------------------
        std::vector<const worldd::QuestDef*> chain;
        {
            bool all = true;
            for (const std::string& name : kChainQuestNames) {
                auto it = by_name.find(name);
                const bool present = it != by_name.end();
                check((std::string("chain quest loaded: ") + name).c_str(), present);
                if (present) chain.push_back(it->second);
                else all = false;
            }
            check("all 10 chain quests present", all && chain.size() == kChainQuestNames.size());
        }

        // ---- (b) Prerequisites form a valid DAG (exist + acyclic). ------------
        {
            bool prereqs_resolve = true;
            for (const worldd::QuestDef* q : chain) {
                for (worldd::QuestId pre : q->prerequisites) {
                    if (by_id.find(pre) == by_id.end()) prereqs_resolve = false;
                }
            }
            check("every prerequisite resolves to a loaded quest", prereqs_resolve);

            // Cycle detection over the whole loaded quest graph (edge q -> prereq).
            // A back-edge during DFS means a cycle.
            std::unordered_map<worldd::QuestId, int> color;  // 0 unseen, 1 active, 2 done
            bool cycle = false;
            std::vector<worldd::QuestId> stack;
            for (const auto& [qid, _] : by_id) {
                if (color[qid] != 0) continue;
                stack.push_back(qid);
                std::vector<std::pair<worldd::QuestId, std::size_t>> frames;
                frames.push_back({qid, 0});
                color[qid] = 1;
                while (!frames.empty()) {
                    auto& [cur, idx] = frames.back();
                    const worldd::QuestDef* qd = by_id[cur];
                    if (qd && idx < qd->prerequisites.size()) {
                        worldd::QuestId next = qd->prerequisites[idx];
                        ++idx;
                        auto nit = by_id.find(next);
                        if (nit == by_id.end()) continue;  // dangling handled above
                        if (color[next] == 1) { cycle = true; break; }
                        if (color[next] == 0) {
                            color[next] = 1;
                            frames.push_back({next, 0});
                        }
                    } else {
                        color[cur] = 2;
                        frames.pop_back();
                    }
                }
                if (cycle) break;
            }
            check("prerequisite graph is acyclic (valid DAG)", !cycle);

            // Real progression: at least one root (no prereqs) and at least one
            // quest gated behind >= 2 prerequisites (a genuine convergence).
            bool has_root = false, has_multi_gate = false;
            for (const worldd::QuestDef* q : chain) {
                if (q->prerequisites.empty()) has_root = true;
                if (q->prerequisites.size() >= 2) has_multi_gate = true;
            }
            check("chain has a root quest (no prerequisites)", has_root);
            check("chain has a convergence quest (>=2 prerequisites)", has_multi_gate);
        }

        // ---- (c) Every objective target resolves + (d) rewards/giver/turn-in. -
        // Also collect the item ids the `collect` objectives require for (e).
        std::set<std::uint32_t> collect_item_ids;
        int kills = 0, collects = 0, delivers = 0, explores = 0;
        {
            bool targets_ok = true, rewards_ok = true, npcs_ok = true, explore_ok = true;
            for (const worldd::QuestDef* q : chain) {
                // giver + turn-in resolve.
                if (!content.npcs->find(q->giver_npc_id)) npcs_ok = false;
                if (!content.npcs->find(q->turn_in_npc())) npcs_ok = false;

                for (const worldd::QuestObjective& o : q->objectives) {
                    switch (o.type) {
                        case worldd::ObjectiveType::kKill:
                            ++kills;
                            if (!content.npcs->find(o.target_npc_id)) targets_ok = false;
                            break;
                        case worldd::ObjectiveType::kCollect:
                            ++collects;
                            if (!content.items->find(o.item_id)) targets_ok = false;
                            else collect_item_ids.insert(o.item_id);
                            break;
                        case worldd::ObjectiveType::kDeliver:
                            ++delivers;
                            if (!content.items->find(o.item_id)) targets_ok = false;
                            if (!content.npcs->find(o.to_npc_id)) targets_ok = false;
                            break;
                        case worldd::ObjectiveType::kExplore: {
                            ++explores;
                            // POI resolves against the zone's `area` rows (WLD-03).
                            db::Result r = conn.execute(
                                "SELECT COUNT(*) FROM area WHERE zone_id = ? AND poi = ?",
                                {db::Param{static_cast<std::int64_t>(o.zone_id)},
                                 db::Param{o.poi}});
                            const bool found = !r.rows.empty() && r.rows[0][0] &&
                                               *r.rows[0][0] != "0";
                            if (!found) explore_ok = false;
                            break;
                        }
                    }
                }

                // rewards resolve (always-granted + choice).
                for (const worldd::QuestRewardItem& ri : q->reward_items)
                    if (!content.items->find(ri.item_id)) rewards_ok = false;
                for (const worldd::QuestRewardItem& ci : q->choice_items)
                    if (!content.items->find(ci.item_id)) rewards_ok = false;
            }
            check("every kill/collect/deliver objective target resolves", targets_ok);
            check("every explore objective POI exists in the zone area rows", explore_ok);
            check("every giver + turn-in NPC resolves", npcs_ok);
            check("every reward item (granted + choice) resolves", rewards_ok);

            // All four M1 objective types are exercised by the chain.
            check("chain exercises kill objectives", kills > 0);
            check("chain exercises collect objectives", collects > 0);
            check("chain exercises deliver objectives", delivers > 0);
            check("chain exercises explore objectives", explores > 0);
        }

        // ---- (e) Loot tables actually drop every `collect` item. --------------
        {
            std::unordered_set<std::uint32_t> droppable;
            for (std::uint32_t creature_id : content.loot->ids()) {
                const loot::LootTable* t = content.loot->find(creature_id);
                if (!t) continue;
                for (const loot::LootGroup& g : t->groups)
                    for (const loot::LootEntry& e2 : g.entries)
                        droppable.insert(e2.item_template_id);
            }
            bool all_droppable = !collect_item_ids.empty();
            for (std::uint32_t item_id : collect_item_ids)
                if (droppable.find(item_id) == droppable.end()) all_droppable = false;
            check("every collect-objective item is dropped by some loot table",
                  all_droppable);
        }

        // Tidy up: drop the throwaway database.
    } catch (const db::DbError& e) {
        std::printf("FAIL: DB error: %s\n", e.what());
        {
            const fs::path drop = scratch / "drop.sql";
            std::ofstream(drop) << "DROP DATABASE IF EXISTS " << dbname << ";\n";
            run_sql_file(client, flags, "", drop);
        }
        return 1;
    }

    {
        const fs::path drop = scratch / "drop.sql";
        std::ofstream(drop) << "DROP DATABASE IF EXISTS " << dbname << ";\n";
        run_sql_file(client, flags, "", drop);
    }

    if (g_fail == 0) {
        std::printf("PASS: all IT-M1 chain consistency + load checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d IT-M1 chain check(s) failed\n", g_fail);
    return 1;
}
