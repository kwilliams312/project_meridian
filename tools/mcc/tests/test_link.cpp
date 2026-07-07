// tools/mcc/tests/test_link.cpp — unit/integration tests for the link stage +
// IF-9 idmap allocator (Tools SAD §2.3 / §2.4). Self-contained (no GoogleTest),
// matching test_format.cpp / test_check_file.cpp style.
//
// Covers the task's required scenarios:
//   (a) refs resolve; a dangling ref -> an L011 error with the right file+path
//   (b) the backlink index is correct (reverse reference graph)
//   (c) idmap determinism — same content -> identical idmap (fresh allocation is
//       fully determined by the content, lexicographic tie-break)
//   (d) idmap stability — adding a new entity preserves existing numeric ids and
//       only appends the new one; deleting one retires (never reuses) its index
//   (e) idmap.lock round-trips: read -> allocate -> write(serialize) -> re-read
//       yields an identical map
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "stages/check.h"
#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/idmap.h"
#include "stages/link.h"
#include "stages/model.h"
#include "stages/parse.h"
#include "stages/validate.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool cond, const std::string& name, const std::string& detail = "") {
    ++g_checks;
    if (cond) {
        std::cout << "  ok   " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL " << name << "\n";
        if (!detail.empty()) std::cout << "       " << detail << "\n";
    }
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

// A unique scratch dir under the system temp for a self-cleaning fixture. The
// per-call counter keeps fixtures distinct within one run without pulling in
// platform headers (matches the repo's minimal-dep test ethos).
fs::path make_scratch(const std::string& tag) {
    static int counter = 0;
    const fs::path base = fs::temp_directory_path() /
                          ("mcc_link_test_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

// Build a ContentModel from a content dir by running the real front stages.
mcc::model::ContentModel build_model(const std::string& content_dir,
                                     mcc::diag::Diagnostics& diags) {
    mcc::model::ContentModel model;
    mcc::stages::discover(content_dir, model);
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    return model;
}

// ---- Fixture content -------------------------------------------------------
// A tiny two-entity pack: a quest that gives via + targets an npc. Adding files
// on top exercises new-entity stability.

void write_pack(const fs::path& root) {
    write_file(root / "mini" / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: mini\n"
               "name: Mini\n"
               "version: 0.1.0\n"
               "content_schema_version: 1\n");
}

void write_npc(const fs::path& root, const std::string& name) {
    write_file(root / "mini" / (name + ".npc.yaml"),
               "schema: meridian/npc@1\n"
               "id: mini:npc." + name + "\n"
               "name: " + name + "\n"
               "level: 3\n");
}

void write_quest_referencing(const fs::path& root, const std::string& qname,
                             const std::string& target_npc) {
    write_file(root / "mini" / (qname + ".quest.yaml"),
               "schema: meridian/quest@1\n"
               "id: mini:quest." + qname + "\n"
               "name: " + qname + "\n"
               "giver: npc." + target_npc + "\n"
               "objectives:\n"
               "  - type: kill\n"
               "    target: npc." + target_npc + "\n"
               "    count: 4\n");
}

// ---- (a) refs resolve; dangling ref -> L011 with file + path ---------------

void test_refs_resolve_and_dangling() {
    // Good corpus: quest refs an npc that exists -> no errors, edges present.
    {
        const fs::path root = make_scratch("resolve");
        write_pack(root);
        write_npc(root, "goblin");
        write_quest_referencing(root, "cull", "goblin");

        mcc::diag::Diagnostics diags;
        auto model = build_model(root.string(), diags);
        // allocate=true writes a fresh idmap.lock so there is no read-only L015
        // drift — a good corpus links cleanly end to end.
        auto linked = mcc::stages::link(model, root.string(), /*allocate=*/true, diags);

        report(diags.error_count() == 0, "resolve: no errors on a good corpus",
               "errors=" + std::to_string(diags.error_count()));
        // Two edges: giver + objectives[0].target both point at mini:npc.goblin.
        report(linked.edges.size() == 2, "resolve: two resolved edges collected",
               "edges=" + std::to_string(linked.edges.size()));
        report(linked.dangling_count == 0, "resolve: zero dangling");
        fs::remove_all(root);
    }

    // Dangling: quest refs an npc that does NOT exist -> one L011 error on the
    // quest file, at the json-path of the offending reference.
    {
        const fs::path root = make_scratch("dangling");
        write_pack(root);
        // No npc file written; the quest still references npc.ghost.
        write_quest_referencing(root, "haunt", "ghost");

        mcc::diag::Diagnostics diags;
        auto model = build_model(root.string(), diags);
        // Clear the validate stage's own L011 so we test link's verdict alone.
        mcc::diag::Diagnostics link_diags;
        auto linked = mcc::stages::link(model, root.string(), /*allocate=*/false, link_diags);

        report(linked.dangling_count == 2, "dangling: both refs reported dangling",
               "dangling=" + std::to_string(linked.dangling_count));

        bool found_l011 = false, right_file = false, right_path = false;
        for (const auto& d : link_diags.items()) {
            if (d.rule == "L011" && d.severity == mcc::diag::Severity::Error) {
                found_l011 = true;
                if (d.file.find("haunt.quest.yaml") != std::string::npos) right_file = true;
                if (d.where == "$.giver" || d.where.rfind("$.objectives[0].target", 0) == 0)
                    right_path = true;
                if (d.message.find("mini:npc.ghost") == std::string::npos)
                    report(false, "dangling: message names the unresolved id",
                           "msg=" + d.message);
            }
        }
        report(found_l011, "dangling: an L011 error is emitted");
        report(right_file, "dangling: L011 names the referencing file");
        report(right_path, "dangling: L011 carries the json-path (where)");
        fs::remove_all(root);
    }
}

// ---- (b) backlinks correct -------------------------------------------------

void test_backlinks() {
    const fs::path root = make_scratch("backlinks");
    write_pack(root);
    write_npc(root, "goblin");
    write_quest_referencing(root, "cull", "goblin");   // -> mini:npc.goblin (x2)
    write_quest_referencing(root, "hunt", "goblin");   // -> mini:npc.goblin (x2)

    mcc::diag::Diagnostics diags;
    auto model = build_model(root.string(), diags);
    auto linked = mcc::stages::link(model, root.string(), /*allocate=*/false, diags);

    const auto it = linked.backlinks.find("mini:npc.goblin");
    report(it != linked.backlinks.end(), "backlinks: goblin has backlinks");
    if (it != linked.backlinks.end()) {
        // De-duped + sorted: two DISTINCT referrers (cull, hunt), each once.
        const std::vector<std::string> expect = {"mini:quest.cull", "mini:quest.hunt"};
        report(it->second == expect, "backlinks: exactly {cull, hunt}, sorted + deduped",
               "got " + std::to_string(it->second.size()) + " referrers");
    }
    // The npc refers to nobody -> it is absent as a backlink key (orphan of refs).
    report(linked.backlinks.find("mini:quest.cull") == linked.backlinks.end(),
           "backlinks: a leaf referrer is not itself a key");
    fs::remove_all(root);
}

// ---- (c) idmap determinism -------------------------------------------------
// Same content -> identical idmap, run twice from a clean slate.

std::string serialize_after_alloc(const std::vector<std::string>& ids) {
    mcc::idmap::IdMap m;
    m.namespace_ = "mini";
    mcc::idmap::allocate(m, ids);
    return mcc::idmap::serialize(m);
}

void test_idmap_determinism() {
    // Insertion order MUST NOT affect the result — allocation is lexicographic.
    std::vector<std::string> a = {"mini:npc.goblin", "mini:item.sword", "mini:quest.cull"};
    std::vector<std::string> b = {"mini:quest.cull", "mini:npc.goblin", "mini:item.sword"};

    const std::string sa = serialize_after_alloc(a);
    const std::string sb = serialize_after_alloc(b);
    report(sa == sb, "determinism: identical idmap regardless of input order");

    // The lexicographic order is item < npc < quest -> indices 1,2,3.
    mcc::idmap::IdMap m;
    m.namespace_ = "mini";
    mcc::idmap::allocate(m, a);
    report(m.map["mini:item.sword"] == 1 && m.map["mini:npc.goblin"] == 2 &&
               m.map["mini:quest.cull"] == 3,
           "determinism: lexicographic index assignment (item=1,npc=2,quest=3)");

    // Numeric id = band*2^20 + index; band 0 -> index == numeric id.
    report(mcc::idmap::numeric_id(0, 2) == 2, "determinism: numeric id for band 0");
    report(mcc::idmap::numeric_id(1, 1) == 1048577, "determinism: numeric id for band 1");
}

// ---- (d) idmap stability ---------------------------------------------------
// Adding an entity preserves existing indices and appends the new one; deleting
// retires (never reuses) an index.

void test_idmap_stability() {
    mcc::idmap::IdMap m;
    m.namespace_ = "mini";
    std::vector<std::string> v1 = {"mini:npc.goblin", "mini:item.sword"};
    mcc::idmap::allocate(m, v1);
    const auto sword_idx = m.map["mini:item.sword"];
    const auto goblin_idx = m.map["mini:npc.goblin"];

    // Add a new entity that sorts BEFORE the existing ones lexicographically
    // ("mini:ability.zap" < "mini:item.sword"). Naive re-sorting would renumber
    // everything; the append-only allocator must NOT — existing indices freeze.
    std::vector<std::string> v2 = {"mini:npc.goblin", "mini:item.sword", "mini:ability.zap"};
    auto r2 = mcc::idmap::allocate(m, v2);

    report(m.map["mini:item.sword"] == sword_idx && m.map["mini:npc.goblin"] == goblin_idx,
           "stability: existing indices unchanged after adding an entity");
    report(m.map["mini:ability.zap"] == 3, "stability: new entity gets the next free index (3)");
    report(r2.newly_allocated == std::vector<std::string>{"mini:ability.zap"},
           "stability: only the new entity is reported allocated");
    report(r2.changed, "stability: allocation reports changed");

    // Re-run on the SAME set: idempotent, nothing changes.
    auto r3 = mcc::idmap::allocate(m, v2);
    report(!r3.changed && r3.newly_allocated.empty(),
           "stability: re-running on unchanged content is idempotent (no change)");

    // Delete the sword: it moves to retired, index frozen, never reused.
    std::vector<std::string> v3 = {"mini:npc.goblin", "mini:ability.zap"};
    auto r4 = mcc::idmap::allocate(m, v3);
    report(m.map.find("mini:item.sword") == m.map.end(),
           "stability: deleted entity leaves the live map");
    report(m.retired["mini:item.sword"] == sword_idx,
           "stability: deleted entity is retired at its frozen index");
    report(r4.retired == std::vector<std::string>{"mini:item.sword"},
           "stability: retirement is reported");

    // Add a brand-new entity: it must NOT reuse the retired index (high-water+1).
    std::vector<std::string> v4 = {"mini:npc.goblin", "mini:ability.zap", "mini:zone.den"};
    mcc::idmap::allocate(m, v4);
    report(m.map["mini:zone.den"] == 4,
           "stability: new index skips the retired one (4, not the freed 2/1)",
           "got " + std::to_string(m.map["mini:zone.den"]));
}

// ---- (e) idmap.lock round-trip ---------------------------------------------

void test_lock_round_trip() {
    mcc::idmap::IdMap m;
    m.namespace_ = "mini";
    m.released_watermark = 2;
    std::vector<std::string> ids = {"mini:npc.goblin", "mini:item.sword", "mini:quest.cull"};
    mcc::idmap::allocate(m, ids);
    // Retire one to exercise the retired: block on the round-trip.
    mcc::idmap::allocate(m, {"mini:npc.goblin", "mini:quest.cull"});

    const std::string text = mcc::idmap::serialize(m);
    mcc::idmap::IdMap parsed;
    std::string err;
    const bool ok = mcc::idmap::parse(text, parsed, err);
    report(ok, "round-trip: serialized lock re-parses", err);
    report(parsed.namespace_ == m.namespace_ && parsed.band == m.band &&
               parsed.released_watermark == m.released_watermark,
           "round-trip: header fields preserved");
    report(parsed.map == m.map, "round-trip: live map preserved");
    report(parsed.retired == m.retired, "round-trip: retired map preserved");

    // Serialize the re-parsed map: byte-identical (stable canonical form).
    report(mcc::idmap::serialize(parsed) == text, "round-trip: re-serialization is byte-identical");
}

// ---- (f) on-disk read -> allocate -> write -> re-read (via link) -----------
// End-to-end: link with allocate=true writes idmap.lock; a second link is a
// pure re-read with no change (proves the DAG-level idempotency).

void test_link_writes_and_is_idempotent() {
    const fs::path root = make_scratch("ondisk");
    write_pack(root);
    write_npc(root, "goblin");
    write_quest_referencing(root, "cull", "goblin");

    const fs::path lock = root / "mini" / "idmap.lock";
    report(!fs::exists(lock), "on-disk: no idmap.lock before first link");

    // First link: allocate + write.
    {
        mcc::diag::Diagnostics diags;
        auto model = build_model(root.string(), diags);
        auto linked = mcc::stages::link(model, root.string(), /*allocate=*/true, diags);
        report(fs::exists(lock), "on-disk: idmap.lock written on first allocating link");
        report(diags.error_count() == 0, "on-disk: first link clean");
        report(linked.idmaps.at("mini").map.size() == 2,
               "on-disk: 2 ids allocated (quest + npc)");
    }
    std::ifstream in1(lock);
    std::ostringstream s1; s1 << in1.rdbuf();

    // Second link (read-only): the lock already covers everything -> no drift,
    // no error, and the file is unchanged.
    {
        mcc::diag::Diagnostics diags;
        auto model = build_model(root.string(), diags);
        mcc::stages::link(model, root.string(), /*allocate=*/false, diags);
        report(diags.error_count() == 0,
               "on-disk: read-only re-link reports no L015 drift (idempotent)",
               "errors=" + std::to_string(diags.error_count()));
    }
    std::ifstream in2(lock);
    std::ostringstream s2; s2 << in2.rdbuf();
    report(s1.str() == s2.str(), "on-disk: idmap.lock byte-identical after re-link");

    fs::remove_all(root);
}

}  // namespace

int main() {
    std::cout << "test_link: link stage + IF-9 idmap allocator (SAD §2.3/§2.4)\n";
    test_refs_resolve_and_dangling();
    test_backlinks();
    test_idmap_determinism();
    test_idmap_stability();
    test_lock_round_trip();
    test_link_writes_and_is_idempotent();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) std::cout << g_failures << " FAILURE(S)\n";
    return g_failures == 0 ? 0 : 1;
}
