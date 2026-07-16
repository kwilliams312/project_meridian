// tools/mcc/tests/test_index.cpp — unit/integration tests for the ID index /
// typed reference pickers / backlinks surface (Tools SAD §2.3/§6, #127).
// Self-contained (no GoogleTest), matching test_link.cpp's style.
//
// Covers the task's required scenarios:
//   (a) the index groups ids by type correctly + resolves id -> numeric/type/file
//   (b) a typed picker returns ONLY valid-type candidates for a ref field
//       (grounded in the schema `*Ref` type expectation; "itemRef"/"item" both)
//   (c) backlinks / find-usages returns the correct referrers for an id (with the
//       field path), and is EMPTY for an unreferenced id + unresolved for unknown
//   (d) the JSON output is stable (deterministic) + parseable (well-formed)
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/index.h"
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

fs::path make_scratch(const std::string& tag) {
    static int counter = 0;
    const fs::path base = fs::temp_directory_path() /
                          ("mcc_index_test_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

// Build a linked IdIndex over a content dir via the real front stages (read-only
// link: allocate=true so a fresh idmap.lock is written and numeric ids exist).
mcc::stages::IdIndex build_index(const std::string& content_dir) {
    mcc::model::ContentModel model;
    mcc::stages::discover(content_dir, model);
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    auto linked = mcc::stages::link(model, content_dir, /*allocate=*/true, diags);
    return mcc::stages::IdIndex::build(model, linked);
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// ---- Fixture corpus --------------------------------------------------------
// A small multi-type pack: an npc, two items, a quest that references the npc
// (giver) + one item (objective) + the other item (reward), and a loot table
// that references an item. Exercises grouping, typed pickers, and backlinks
// with multiple referrers / multiple usages from one referrer.

void write_corpus(const fs::path& root) {
    write_file(root / "t" / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: t\n"
               "name: T\n"
               "version: 0.1.0\n"
               "content_schema_version: 1\n");
    write_file(root / "t" / "goblin.npc.yaml",
               "schema: meridian/npc@2\n"
               "id: t:npc.goblin\n"
               "name: Goblin\n"
               "level: 3\n");
    write_file(root / "t" / "ear.item.yaml",
               "schema: meridian/item@2\n"
               "id: t:item.ear\n"
               "name: Goblin Ear\n");
    write_file(root / "t" / "sword.item.yaml",
               "schema: meridian/item@2\n"
               "id: t:item.sword\n"
               "name: Sword\n");
    // Quest: giver -> npc.goblin, objective collect -> item.ear (x1 path),
    // reward -> item.sword. Two distinct items referenced from distinct fields.
    write_file(root / "t" / "cull.quest.yaml",
               "schema: meridian/quest@1\n"
               "id: t:quest.cull\n"
               "name: Cull\n"
               "giver: npc.goblin\n"
               "objectives:\n"
               "  - type: collect\n"
               "    item: item.ear\n"
               "    count: 4\n"
               "rewards:\n"
               "  items:\n"
               "    - item: item.sword\n");
    // Loot table also references item.ear -> ear has TWO distinct referrers.
    write_file(root / "t" / "drops.loot.yaml",
               "schema: meridian/loot@1\n"
               "id: t:loot.drops\n"
               "entries:\n"
               "  - item: item.ear\n"
               "    chance_pct: 50\n");
}

// ---- (a) grouping + resolution ---------------------------------------------

void test_grouping_and_resolution() {
    const fs::path root = make_scratch("group");
    write_corpus(root);
    const auto idx = build_index(root.string());

    // Types present: npc, item, quest, loot (sorted). Grouping is correct.
    const std::vector<std::string> types = idx.types();
    report(types == std::vector<std::string>({"item", "loot", "npc", "quest"}),
           "grouping: distinct types sorted {item,loot,npc,quest}",
           "got " + std::to_string(types.size()) + " types");

    // by_type groups correctly: two items, one npc.
    const std::vector<std::string> items = idx.by_type("item");
    report(items == std::vector<std::string>({"t:item.ear", "t:item.sword"}),
           "grouping: item group == {ear, sword} sorted");
    report(idx.by_type("npc") == std::vector<std::string>({"t:npc.goblin"}),
           "grouping: npc group == {goblin}");
    report(idx.by_type("spawn").empty(), "grouping: absent type -> empty group");

    // resolve() gives type + numeric + file; numeric ids are non-zero + distinct.
    const auto ear = idx.resolve("t:item.ear");
    const auto sword = idx.resolve("t:item.sword");
    report(ear.has_value() && ear->type == "item" && ear->namespace_ == "t",
           "resolve: fully-qualified id -> type+namespace");
    report(ear.has_value() && ear->numeric_id != 0,
           "resolve: entry carries a non-zero IF-9 numeric id");
    report(ear && sword && ear->numeric_id != sword->numeric_id,
           "resolve: distinct ids have distinct numeric ids");
    report(ear.has_value() && ear->file.find("ear.item.yaml") != std::string::npos,
           "resolve: entry carries the source file");
    report(!ear->is_asset, "resolve: a content id is not flagged is_asset");

    // Bare id resolves against the sole namespace.
    const auto bare = idx.resolve("item.ear");
    report(bare.has_value() && bare->id == "t:item.ear",
           "resolve: a bare id defaults to the sole namespace");

    // Unknown id -> nullopt.
    report(!idx.resolve("t:item.nonexistent").has_value(),
           "resolve: unknown id -> nullopt");

    fs::remove_all(root);
}

// ---- (b) typed pickers return only valid-type candidates -------------------

void test_typed_pickers() {
    const fs::path root = make_scratch("picker");
    write_corpus(root);
    const auto idx = build_index(root.string());

    // pickable("item") returns exactly the item ids — no npc, no quest, no loot.
    const std::vector<std::string> item_candidates = idx.pickable("item");
    report(item_candidates == std::vector<std::string>({"t:item.ear", "t:item.sword"}),
           "picker: pickable(item) == the two item ids");
    report(!contains(item_candidates, "t:npc.goblin"),
           "picker: an item picker never offers an npc (typed by schema)");
    report(!contains(item_candidates, "t:quest.cull"),
           "picker: an item picker never offers a quest");

    // The schema `$defs` name ("itemRef") normalizes to the same candidates —
    // this is the exact type expectation a schema itemRef field carries.
    report(idx.pickable("itemRef") == item_candidates,
           "picker: schema ref-def name 'itemRef' == 'item' candidates");

    // pickable("npc") is the npc set; a quest's giver field (an npcRef) would
    // pick from exactly this.
    report(idx.pickable("npcRef") == std::vector<std::string>({"t:npc.goblin"}),
           "picker: pickable(npcRef) == the npc set");

    // An unknown / absent ref type -> empty (no candidates), not a crash.
    report(idx.pickable("bogusRef").empty(),
           "picker: unknown ref type -> empty candidate set");
    report(idx.pickable("vendor").empty(),
           "picker: a type with no ids -> empty candidate set");

    fs::remove_all(root);
}

// ---- (c) backlinks / find-usages -------------------------------------------

void test_backlinks() {
    const fs::path root = make_scratch("backlinks");
    write_corpus(root);
    const auto idx = build_index(root.string());

    // item.ear is referenced by BOTH the quest (objective) and the loot table.
    const auto ear_refs = idx.backlinks("t:item.ear");
    report(ear_refs.size() == 2, "backlinks: item.ear has two referrers",
           "got " + std::to_string(ear_refs.size()));
    // Sorted by (from, where): loot < quest lexicographically.
    if (ear_refs.size() == 2) {
        report(ear_refs[0].from == "t:loot.drops" && ear_refs[0].where == "$.entries[0].item",
               "backlinks: referrer carries the field path (loot entry)");
        report(ear_refs[1].from == "t:quest.cull" &&
                   ear_refs[1].where == "$.objectives[0].item",
               "backlinks: referrer carries the field path (quest objective)");
    }

    // npc.goblin is referenced only by the quest's giver field.
    const auto goblin_refs = idx.backlinks("t:npc.goblin");
    report(goblin_refs.size() == 1 && goblin_refs[0].from == "t:quest.cull" &&
               goblin_refs[0].where == "$.giver",
           "backlinks: npc.goblin <- quest.cull at $.giver");

    // A bare id resolves for backlinks too.
    report(idx.backlinks("item.sword").size() == 1,
           "backlinks: bare id 'item.sword' resolves (reward reference)");

    // The quest references others but nobody references IT -> empty (orphan).
    report(idx.backlinks("t:quest.cull").empty(),
           "backlinks: an unreferenced id has zero referrers (empty)");

    // An unknown id -> empty (resolve() distinguishes exists-but-orphan).
    report(idx.backlinks("t:item.ghost").empty(),
           "backlinks: unknown id -> empty");

    fs::remove_all(root);
}

// ---- (d) JSON output is stable + parseable ---------------------------------

// A dependency-free structural JSON sanity check: balanced braces/brackets, and
// balanced double-quotes outside of escapes. Not a full parser, but catches the
// malformed-output regressions the contract cares about (trailing commas break
// real parsers, but our emitters place separators before elements so a stray
// trailing comma cannot occur — this guards brace/quote balance + key presence).
bool json_balanced(const std::string& s) {
    int braces = 0, brackets = 0;
    bool in_str = false, esc = false;
    for (const char c : s) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{') ++braces;
        else if (c == '}') --braces;
        else if (c == '[') ++brackets;
        else if (c == ']') --brackets;
        if (braces < 0 || brackets < 0) return false;
    }
    return braces == 0 && brackets == 0 && !in_str;
}

void test_json_stable_and_parseable() {
    const fs::path root = make_scratch("json");
    write_corpus(root);
    const auto idx = build_index(root.string());

    // Index JSON: well-formed, carries the schema tag + counts + a known id.
    std::ostringstream i1;
    mcc::stages::render_index_json(idx, i1);
    const std::string index_json = i1.str();
    report(json_balanced(index_json), "json: index JSON is brace/quote balanced");
    report(index_json.find("\"schema\": \"meridian/id-index@1\"") != std::string::npos,
           "json: index carries the schema tag");
    report(index_json.find("\"id_count\": 5") != std::string::npos,
           "json: index reports id_count 5 (npc+2 items+quest+loot)");
    report(index_json.find("\"t:item.ear\"") != std::string::npos,
           "json: index lists a known id");

    // Determinism: re-rendering the same index is byte-identical.
    std::ostringstream i2;
    mcc::stages::render_index_json(idx, i2);
    report(i2.str() == index_json, "json: index render is deterministic (byte-identical)");

    // Rebuilding the index from scratch over the same content is byte-identical
    // (the whole pipeline is deterministic, not just the serializer).
    const auto idx2 = build_index(root.string());
    std::ostringstream i3;
    mcc::stages::render_index_json(idx2, i3);
    report(i3.str() == index_json, "json: rebuilt index is byte-identical (pipeline determinism)");

    // Pickable JSON: well-formed, correct ref_type/type/count.
    std::ostringstream p1;
    mcc::stages::render_pickable_json(idx, "itemRef", p1);
    const std::string pick_json = p1.str();
    report(json_balanced(pick_json), "json: pickable JSON is balanced");
    report(pick_json.find("\"ref_type\": \"itemRef\"") != std::string::npos &&
               pick_json.find("\"type\": \"item\"") != std::string::npos,
           "json: pickable echoes ref_type + normalized type");
    report(pick_json.find("\"count\": 2") != std::string::npos,
           "json: pickable reports 2 candidates");

    // Unknown ref type -> ok:false, count:0, still balanced.
    std::ostringstream p2;
    mcc::stages::render_pickable_json(idx, "bogusRef", p2);
    report(json_balanced(p2.str()) &&
               p2.str().find("\"ok\": false") != std::string::npos,
           "json: unknown pickable ref type -> ok:false, still valid JSON");

    // Backlinks JSON: resolved id has entry + referrers.
    std::ostringstream b1;
    mcc::stages::render_backlinks_json(idx, "t:item.ear", b1);
    const std::string back_json = b1.str();
    report(json_balanced(back_json), "json: backlinks JSON is balanced");
    report(back_json.find("\"resolved\": true") != std::string::npos &&
               back_json.find("\"referrer_count\": 2") != std::string::npos,
           "json: backlinks reports resolved + referrer_count");
    report(back_json.find("\"where\": \"$.giver\"") != std::string::npos ||
               back_json.find("$.objectives[0].item") != std::string::npos,
           "json: backlinks carries a referencing field path");

    // Backlinks for an unknown id -> resolved:false, empty referrers, balanced.
    std::ostringstream b2;
    mcc::stages::render_backlinks_json(idx, "t:item.ghost", b2);
    report(json_balanced(b2.str()) &&
               b2.str().find("\"resolved\": false") != std::string::npos &&
               b2.str().find("\"referrer_count\": 0") != std::string::npos,
           "json: unknown-id backlinks -> resolved:false, 0 referrers");

    fs::remove_all(root);
}

// ---- (e) asset ids are indexed + typed too ---------------------------------
// Assets (art/mus/sfx/amb) get IF-9 ids and are pickable by their type (an
// artRef field picks from art ids) — the same typed-picker mechanism.

void test_assets_indexed() {
    const fs::path root = make_scratch("assets");
    write_file(root / "t" / "pack.yaml",
               "schema: meridian/pack@1\nnamespace: t\nname: T\nversion: 0.1.0\n"
               "content_schema_version: 1\n");
    write_file(root / "t" / "icon.asset.yaml",
               "schema: meridian/asset@1\n"
               "id: t:art.icon.sword\n"
               "kind: texture\n"
               "source: art/icon_sword.png\n");
    const auto idx = build_index(root.string());

    const auto e = idx.resolve("t:art.icon.sword");
    report(e.has_value() && e->type == "art" && e->is_asset,
           "assets: an asset id is typed 'art' + flagged is_asset");
    report(idx.pickable("artRef") == std::vector<std::string>({"t:art.icon.sword"}),
           "assets: pickable(artRef) offers the art asset id");
    fs::remove_all(root);
}

}  // namespace

int main() {
    std::cout << "test_index: ID index + typed pickers + backlinks (SAD §2.3/§6)\n";
    test_grouping_and_resolution();
    test_typed_pickers();
    test_backlinks();
    test_json_stable_and_parseable();
    test_assets_indexed();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) std::cout << g_failures << " FAILURE(S)\n";
    return g_failures == 0 ? 0 : 1;
}
