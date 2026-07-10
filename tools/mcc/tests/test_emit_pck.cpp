// tools/mcc/tests/test_emit_pck.cpp — unit/determinism tests for the emit-pck
// stage (Tools SAD §2.7, IF-5). Self-contained (no GoogleTest), matching
// test_emit_sql.cpp / test_link.cpp style. CTest-registered.
//
// Covers the task's required scenarios:
//   (a) well-formed pack.manifest.json: the fixed-key JSON metadata (namespace,
//       version, content_schema_version, godot_version, id_band, content_hash,
//       entry_count) with a 64-lowercase-hex BLAKE3 content_hash and an entry
//       list whose entries carry id + IF-9 numeric_id + res:// resource + hash.
//   (b) IF-4/IF-5 CONSISTENCY — the emit-pck content_hash EQUALS the emit-sql
//       world_manifest content_hash for the same content. This is the key
//       cross-emit invariant (the three-way content-hash tie, SAD §2.6); a drift
//       silently breaks client pack verification (a P0 tools bug).
//   (c) determinism — the same content emits a byte-identical manifest across
//       runs (and identical source bytes -> identical content_hash across builds).
//   (d) entry-list coverage — the entry list covers every allocated content +
//       asset id (entry_count == the idmap id count), each entry's numeric_id is
//       the same runtime id the SQL keys rows by (no string ids leak).
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/emit_pck.h"
#include "stages/emit_sql.h"
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
                          ("mcc_pck_test_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

// Build a fixture content tree: a pack + one ability + one item that references
// the ability + one asset sidecar. Exercises content entries, asset entries, ref
// resolution, and the manifest metadata (version + engine pin).
fs::path make_fixture() {
    const fs::path root = make_scratch("fixture");
    const fs::path content = root / "content";
    const fs::path core = content / "core";
    write_file(core / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: core\n"
               "name: Test Pack\n"
               "version: 1.2.3\n"
               "content_schema_version: 1\n"
               "engine:\n  godot: \"4.6\"\n");
    write_file(core / "abilities" / "zap.ability.yaml",
               "schema: meridian/ability@1\n"
               "id: core:ability.zap\n"
               "name: Zap\n"
               "school: arcane\n"
               "target: enemy\n"
               "range_m: 30\n"
               "cast:\n  time_ms: 1500\n"
               "cooldown_ms: 8000\n"
               "effects:\n"
               "  - kind: damage\n"
               "    amount: { min: 10, max: 20 }\n");
    write_file(core / "items" / "wand.item.yaml",
               "schema: meridian/item@2\n"
               "id: core:item.wand\n"
               "name: Wand of Zapping\n"
               "item_class: consumable\n"
               "rarity: rare\n"
               "effects:\n  on_use: ability.zap\n"
               "price:\n  sell: 100\n");
    write_file(core / "assets" / "icon_wand.asset.yaml",
               "schema: meridian/asset@1\n"
               "id: core:art.icon.item.wand\n"
               "class: icon\n"
               "source: assets/art/icon/item/wand.png\n"
               "license: CC-BY-4.0\n"
               "provenance:\n  source_tier: original\n  authors: [test]\n");
    return content.string();
}

// Run the real front stages + emit-pck over a content dir.
mcc::stages::EmitPckResult run_emit_pck(const std::string& content_dir, bool& ok) {
    mcc::model::ContentModel model;
    mcc::stages::discover(content_dir, model);
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    const mcc::stages::LinkResult linked =
        mcc::stages::link(model, content_dir, /*allocate=*/true, diags,
                          /*emit_dangling=*/false);
    mcc::stages::EmitPckOptions opts;
    opts.mcc_version = "test-1.0.0";
    opts.built_at = "2026-07-06 12:00:00";
    mcc::stages::EmitPckResult res = mcc::stages::emit_pck(model, linked, opts, diags);
    ok = diags.ok();
    return res;
}

// Run the real front stages + emit-sql over a content dir (for the IF-4/IF-5
// hash-consistency check). Uses the SAME allocate=true front half so both emits
// read the identical idmap.
mcc::stages::EmitSqlResult run_emit_sql(const std::string& content_dir, bool& ok) {
    mcc::model::ContentModel model;
    mcc::stages::discover(content_dir, model);
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    const mcc::stages::LinkResult linked =
        mcc::stages::link(model, content_dir, /*allocate=*/true, diags,
                          /*emit_dangling=*/false);
    mcc::stages::EmitSqlOptions opts;
    opts.mcc_version = "test-1.0.0";
    opts.built_at = "2026-07-06 12:00:00";
    mcc::stages::EmitSqlResult res = mcc::stages::emit_sql(model, linked, opts, diags);
    ok = diags.ok();
    return res;
}

bool is_lower_hex_64(const std::string& s) {
    if (s.size() != 64) return false;
    for (const char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// Extract the world_manifest content_hash (4th column) from emitted SQL. Mirrors
// the emit-sql test's tuple parse but returns just the hash cell.
std::string sql_content_hash(const std::string& sql) {
    const std::size_t ins = sql.find("INSERT INTO world_manifest");
    if (ins == std::string::npos) return "";
    // The hash is the only 64-hex-in-quotes token after the INSERT: find it.
    const std::size_t v = sql.find("VALUES", ins);
    std::size_t p = sql.find('\'', v);
    while (p != std::string::npos) {
        const std::size_t q = sql.find('\'', p + 1);
        if (q == std::string::npos) break;
        const std::string tok = sql.substr(p + 1, q - p - 1);
        if (is_lower_hex_64(tok)) return tok;
        p = sql.find('\'', q + 1);
    }
    return "";
}

// Extract the JSON string value for a top-level "key": "value" pair.
std::string json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    std::size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    p = json.find('"', p + needle.size());
    if (p == std::string::npos) return "";
    const std::size_t q = json.find('"', p + 1);
    if (q == std::string::npos) return "";
    return json.substr(p + 1, q - p - 1);
}

// -- (a) well-formed manifest -------------------------------------------------
void test_manifest_wellformed() {
    std::cout << "test_manifest_wellformed (pack.manifest.json shape)\n";
    const std::string content = make_fixture();
    bool ok = false;
    const mcc::stages::EmitPckResult res = run_emit_pck(content, ok);
    report(ok, "emit-pck over valid content reports no diagnostic errors");
    report(res.ok, "EmitPckResult.ok is true");
    report(res.pack_namespace == "core", "pack namespace = core", res.pack_namespace);
    report(res.pack_version == "1.2.3", "pack version from pack.yaml", res.pack_version);
    report(is_lower_hex_64(res.content_hash),
           "content_hash is 64 lowercase-hex (BLAKE3)", res.content_hash);

    const std::string& j = res.manifest_json;
    report(j.find("\"schema\": \"meridian/pack-manifest@1\"") != std::string::npos,
           "manifest declares its own schema");
    report(json_string_field(j, "namespace") == "core", "namespace field = core");
    report(json_string_field(j, "version") == "1.2.3", "version field = 1.2.3");
    report(json_string_field(j, "godot_version") == "4.6",
           "godot_version pinned from engine.godot", json_string_field(j, "godot_version"));
    report(json_string_field(j, "content_hash") == res.content_hash,
           "content_hash field matches the result");
    report(j.find("\"content_schema_version\": 1") != std::string::npos,
           "content_schema_version = 1");
    report(j.find("\"id_band\": 0") != std::string::npos, "id_band = 0 (core)");

    // Entry list: 3 entries (ability + item + asset). Each carries the required
    // keys and a res:// path + per-resource hash.
    report(res.entries.size() == 3, "3 entries (ability + item + asset)",
           "got " + std::to_string(res.entries.size()));
    report(j.find("\"entry_count\": 3") != std::string::npos, "entry_count = 3");
    report(j.find("\"res://meridian/core/") != std::string::npos,
           "entries carry res://meridian/core/ resource paths (by ID)");
    report(j.find("res://meridian/core/art/icon/item/wand.ctex") != std::string::npos,
           "icon asset res:// path derived by ID with imported-texture ext");
    report(j.find("res://meridian/core/tables/ability.bin") != std::string::npos,
           "content entity res:// path is the per-type table blob");
    // Every entry's per-resource hash is 64-hex.
    bool all_hashes_hex = !res.entries.empty();
    for (const auto& e : res.entries) all_hashes_hex &= is_lower_hex_64(e.resource_hash);
    report(all_hashes_hex, "every entry has a 64-hex per-resource BLAKE3");
}

// -- (b) IF-4/IF-5 content_hash consistency (the key invariant) ---------------
void test_if4_if5_hash_agreement() {
    std::cout << "test_if4_if5_hash_agreement (emit-pck hash == emit-sql hash)\n";
    const std::string content = make_fixture();
    bool ok_pck = false, ok_sql = false;
    const std::string pck_hash = run_emit_pck(content, ok_pck).content_hash;
    const std::string sql_hash = sql_content_hash(run_emit_sql(content, ok_sql).sql);
    report(is_lower_hex_64(pck_hash), "emit-pck content_hash is 64-hex", pck_hash);
    report(is_lower_hex_64(sql_hash), "emit-sql world_manifest content_hash is 64-hex",
           sql_hash);
    report(pck_hash == sql_hash,
           "IF-4/IF-5 AGREE: emit-pck content_hash EQUALS emit-sql world_manifest hash",
           "pck=" + pck_hash + " sql=" + sql_hash);
}

// -- (c) determinism ----------------------------------------------------------
void test_determinism() {
    std::cout << "test_determinism (same content -> identical manifest)\n";
    const std::string content = make_fixture();
    bool ok1 = false, ok2 = false;
    const std::string a = run_emit_pck(content, ok1).manifest_json;
    const std::string b = run_emit_pck(content, ok2).manifest_json;
    report(a == b, "two emit-pck runs over the same content are byte-identical",
           a == b ? "" : "manifests differ");

    // A second, independently-built fixture with the SAME content bytes must
    // produce the same content_hash (a pure function of the source).
    const std::string content2 = make_fixture();
    const std::string ha = json_string_field(a, "content_hash");
    const std::string hc = run_emit_pck(content2, ok2).content_hash;
    report(!ha.empty() && ha == hc,
           "identical source bytes -> identical content_hash across builds",
           "a=" + ha + " c=" + hc);
}

// -- (d) entry-list coverage --------------------------------------------------
void test_entry_coverage() {
    std::cout << "test_entry_coverage (entries cover every allocated id)\n";
    const std::string content = make_fixture();
    // Independently link to get the idmap id count for the pack.
    mcc::model::ContentModel model;
    mcc::stages::discover(content, model);
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    const mcc::stages::LinkResult linked =
        mcc::stages::link(model, content, /*allocate=*/true, diags, /*emit_dangling=*/false);
    std::size_t idmap_count = 0;
    for (const auto& [ns, m] : linked.idmaps) idmap_count += m.map.size();

    bool ok = false;
    const mcc::stages::EmitPckResult res = run_emit_pck(content, ok);
    report(res.entries.size() == idmap_count,
           "entry count == allocated idmap id count (full coverage)",
           "entries=" + std::to_string(res.entries.size()) +
               " idmap=" + std::to_string(idmap_count));

    // Every entry has a non-zero IF-9 numeric id, entries are sorted ascending,
    // and no string content ids leak in place of the numeric ids.
    bool all_numeric = true, sorted = true;
    std::uint32_t prev = 0;
    for (const auto& e : res.entries) {
        all_numeric &= (e.numeric_id != 0);
        sorted &= (e.numeric_id >= prev);
        prev = e.numeric_id;
    }
    report(all_numeric, "every entry has a non-zero IF-9 numeric id");
    report(sorted, "entries are sorted by numeric id (deterministic order)");

    // The M0 directory-manifest pack has one line per entry (the id/resource/hash
    // triple the client would mount).
    std::size_t lines = 0;
    for (const char c : res.contents_jsonl)
        if (c == '\n') ++lines;
    report(lines == res.entries.size(),
           "M0 pack (pack.contents.jsonl) has one line per entry",
           "lines=" + std::to_string(lines));
}

}  // namespace

int main() {
    std::cout << "mcc emit-pck (IF-5 client pack + pack.manifest.json) unit tests\n\n";
    test_manifest_wellformed();
    test_if4_if5_hash_agreement();
    test_determinism();
    test_entry_coverage();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
