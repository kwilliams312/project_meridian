// tools/mcc/tests/test_emit_sql.cpp — unit/determinism tests for the emit-sql
// stage + the vendored BLAKE3 (Tools SAD §2.6, IF-4). Self-contained (no
// GoogleTest), matching test_link.cpp / test_format.cpp style. CTest-registered.
//
// Covers the task's required scenarios:
//   (a) BLAKE3 correctness — the vendored hasher matches published BLAKE3 test
//       vectors (empty input + a known short input) so content_hash is a REAL
//       BLAKE3, not a placeholder.
//   (b) emit-sql on linked content produces well-formed SQL: a preamble with
//       FOREIGN_KEY_CHECKS=0, exactly one world_manifest row whose content_hash
//       is 64 lowercase-hex and whose schema_version is 1 (worldd's supported
//       major), and content-table INSERTs keyed by IF-9 numeric ids.
//   (c) determinism — the same content emits byte-identical SQL across runs.
//   (d) ref resolution — a *Ref string becomes the referent's numeric id.
//   (e) the emitted manifest is shaped exactly as worldd (#89) accepts: 7
//       columns, 64-lowercase-hex hash, schema_version == 1 — verified by
//       re-parsing the emitted VALUES line (the same fields worldd's
//       verify_world_manifest checks).
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "hash/blake3.h"
#include "stages/diagnostics.h"
#include "stages/discover.h"
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
                          ("mcc_emit_test_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

// Build a fixture content tree: a pack + one ability + one item that references
// the ability (effects.on_use) + one npc that references both a loot table and
// the ability. Small but exercises ref resolution, child tables, and manifest.
fs::path make_fixture() {
    const fs::path root = make_scratch("fixture");
    const fs::path content = root / "content";
    const fs::path core = content / "core";
    write_file(core / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: core\n"
               "name: Test Pack\n"
               "version: 1.2.3\n"
               "content_schema_version: 1\n");
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
               "schema: meridian/item@1\n"
               "id: core:item.wand\n"
               "name: Wand of Zapping\n"
               "item_class: consumable\n"
               "rarity: rare\n"
               "effects:\n  on_use: ability.zap\n"
               "price:\n  sell: 100\n");
    return content.string();
}

// Run the real front stages + emit-sql over a content dir. Returns the SQL and
// captures diagnostics ok().
mcc::stages::EmitSqlResult run_emit(const std::string& content_dir, bool& ok) {
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

// Extract the single-quoted fields + bare numeric fields of the world_manifest
// VALUES tuple. Returns the raw comma-separated cell strings (trimmed).
std::vector<std::string> parse_manifest_values(const std::string& sql) {
    const std::string marker = "INSERT INTO world_manifest";
    const std::size_t ins = sql.find(marker);
    if (ins == std::string::npos) return {};
    const std::size_t vpos = sql.find("VALUES", ins);
    if (vpos == std::string::npos) return {};
    const std::size_t open = sql.find('(', vpos);
    const std::size_t close = sql.find(')', open);
    if (open == std::string::npos || close == std::string::npos) return {};
    std::string inner = sql.substr(open + 1, close - open - 1);

    std::vector<std::string> cells;
    std::string cur;
    bool in_str = false;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (in_str) {
            if (c == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') {
                cur += c;
                ++i;
            } else if (c == '\'') {
                in_str = false;
            } else {
                cur += c;
            }
        } else if (c == '\'') {
            in_str = true;
        } else if (c == ',') {
            cells.push_back(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur += c;
        }
    }
    if (!cur.empty()) cells.push_back(cur);
    return cells;
}

// -- (a) BLAKE3 correctness ---------------------------------------------------
void test_blake3() {
    std::cout << "test_blake3 (vendored hasher matches published vectors)\n";
    // Empty input.
    report(mcc::hash::blake3_hex(nullptr, 0) ==
               "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
           "BLAKE3(\"\") matches the published empty-input vector");
    // "abc".
    const std::string abc = "abc";
    report(mcc::hash::blake3_hex(abc.data(), abc.size()) ==
               "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
           "BLAKE3(\"abc\") matches the published vector");
    // Incremental == one-shot.
    mcc::hash::Blake3 h;
    h.update("a", 1);
    h.update("b", 1);
    h.update("c", 1);
    report(h.hex() == mcc::hash::blake3_hex(abc.data(), abc.size()),
           "incremental update() equals one-shot for the same bytes");
}

// -- (b) well-formed SQL + manifest row ---------------------------------------
void test_emit_wellformed() {
    std::cout << "test_emit_wellformed (SQL + manifest row shape)\n";
    const std::string content = make_fixture();
    bool ok = false;
    const mcc::stages::EmitSqlResult res = run_emit(content, ok);
    report(ok, "emit-sql over valid content reports no diagnostic errors");
    report(res.ok, "EmitSqlResult.ok is true");
    report(res.manifest_rows == 1, "exactly one world_manifest row emitted",
           "got " + std::to_string(res.manifest_rows));
    report(res.content_rows > 0, "content rows emitted",
           "got " + std::to_string(res.content_rows));

    const std::string& sql = res.sql;
    report(sql.find("SET FOREIGN_KEY_CHECKS = 0;") != std::string::npos,
           "preamble disables FK checks for the bulk load");
    report(sql.find("SET FOREIGN_KEY_CHECKS = 1;") != std::string::npos,
           "epilogue restores FK checks");
    report(sql.find("INSERT INTO world_manifest") != std::string::npos,
           "world_manifest INSERT present");
    report(sql.find("INSERT INTO ability") != std::string::npos,
           "ability content INSERT present");
    report(sql.find("INSERT INTO item_template") != std::string::npos,
           "item_template content INSERT present");
    report(sql.find("ability_effect") != std::string::npos,
           "child table (ability_effect) INSERT present");

    // The manifest tuple: (pack_namespace, pack_version, id_band, content_hash,
    // schema_version, mcc_version, built_at).
    const std::vector<std::string> cells = parse_manifest_values(sql);
    report(cells.size() == 7, "manifest row has 7 columns (worldd's read shape)",
           "got " + std::to_string(cells.size()));
    if (cells.size() == 7) {
        report(cells[0] == "core", "pack_namespace = core", cells[0]);
        report(cells[1] == "1.2.3", "pack_version from pack.yaml", cells[1]);
        report(is_lower_hex_64(cells[3]), "content_hash is 64 lowercase-hex (BLAKE3)",
               cells[3]);
        report(cells[4] == "1", "schema_version = 1 (worldd's supported major)",
               cells[4]);
        report(cells[5] == "test-1.0.0", "mcc_version stamped", cells[5]);
        report(cells[6] == "2026-07-06 12:00:00", "built_at is the parameterized ts",
               cells[6]);
    }
}

// -- (c) determinism ----------------------------------------------------------
void test_determinism() {
    std::cout << "test_determinism (same content -> identical SQL)\n";
    const std::string content = make_fixture();
    bool ok1 = false, ok2 = false;
    const std::string a = run_emit(content, ok1).sql;
    const std::string b = run_emit(content, ok2).sql;
    report(a == b, "two emit runs over the same content are byte-identical",
           a == b ? "" : "outputs differ");

    // A second, independently-built fixture with the SAME content bytes must
    // produce the same content_hash (the hash is a pure function of the source).
    const std::string content2 = make_fixture();
    const std::string c = run_emit(content2, ok2).sql;
    const auto ca = parse_manifest_values(a);
    const auto cc = parse_manifest_values(c);
    report(!ca.empty() && !cc.empty() && ca[3] == cc[3],
           "identical source bytes -> identical content_hash across builds");
}

// -- (d) ref resolution -------------------------------------------------------
void test_ref_resolution() {
    std::cout << "test_ref_resolution (*Ref -> IF-9 numeric id)\n";
    const std::string content = make_fixture();
    bool ok = false;
    const std::string sql = run_emit(content, ok).sql;
    // ability.zap and item.wand: lexicographic idmap -> ability.zap index 1,
    // item.wand index 2 (band 0 => numeric 1 and 2). The item's effect_on_use_id
    // must be the ability's numeric id (1), NOT the string "ability.zap".
    report(sql.find("core:ability.zap") == std::string::npos,
           "no string content ids leak into the SQL (numeric keys only)");
    // The ability row's id column is 1; the item's effect_on_use_id references it.
    // item_template columns: id,name,...,effect_on_use_id,... so the item row
    // must contain a numeric reference to the ability id.
    const std::size_t item_ins = sql.find("INSERT INTO item_template");
    report(item_ins != std::string::npos, "item_template insert present");
    // item_template columns end: ..., effect_on_use_id, price_sell, price_buy,
    // visual_icon_id, visual_model_id. The wand's on_use -> ability.zap (index 1,
    // band 0 => numeric id 1); price.sell = 100. So the wand row's tail must be
    // "..., 1, 100, NULL, NULL, NULL)". Assert that exact numeric-ref tail — this
    // fails if the ref were emitted as a string or resolved to the wrong id.
    const std::size_t item_row = sql.find("'Wand of Zapping'");
    report(item_row != std::string::npos, "wand row present");
    if (item_row != std::string::npos) {
        const std::size_t row_end = sql.find(')', item_row);
        const std::string row = sql.substr(item_row, row_end - item_row);
        // effect_on_use_id must be 1 (ability.zap's numeric id), not a string.
        report(row.find(", 1, 100,") != std::string::npos,
               "wand effect_on_use_id = 1 (ability.zap's IF-9 numeric id), price_sell=100",
               row);
    }
}

}  // namespace

int main() {
    std::cout << "mcc emit-sql (IF-4 world DB SQL + manifest) unit tests\n\n";
    test_blake3();
    test_emit_wellformed();
    test_determinism();
    test_ref_resolution();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
