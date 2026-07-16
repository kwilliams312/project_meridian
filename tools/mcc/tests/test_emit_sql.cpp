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
               "schema: meridian/item@2\n"
               "id: core:item.wand\n"
               "name: Wand of Zapping\n"
               "item_class: consumable\n"
               "rarity: rare\n"
               "effects:\n  on_use: ability.zap\n"
               "price:\n  sell: 100\n");
    // A base attribute (SP2.4 #694) — the kernel vocabulary emits to the `attribute`
    // table keyed by its contentId ref, carrying the primary/derived `kind`.
    write_file(core / "attributes" / "strength.attribute.yaml",
               "schema: meridian/attribute@1\n"
               "id: core:attribute.strength\n"
               "name: Strength\n"
               "kind: primary\n");
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
    // SP2.1: effects[] rides as a generic canonical-JSON payload on the ability
    // row (the per-kind ability_effect child tables are retired). The zap fixture
    // has one damage effect (amount 10-20); its effects_json must be the canonical
    // (keys sorted) JSON array, embedded as a single-quoted SQL string literal.
    report(sql.find("ability_effect") == std::string::npos,
           "retired child tables (ability_effect*) no longer emitted");
    report(sql.find(R"('[{"amount":{"max":20,"min":10},"kind":"damage"}]')") !=
               std::string::npos,
           "ability effects_json is canonical (keys sorted) JSON on the ability row");
    // SP2.4 #694: the base attribute vocabulary emits to the `attribute` table,
    // keyed by the contentId ref, carrying its primary/derived kind.
    report(sql.find("INSERT INTO attribute (attr_ref, content_id, name, kind)") !=
               std::string::npos,
           "attribute content INSERT present");
    report(sql.find("'core:attribute.strength'") != std::string::npos &&
               sql.find("'Strength', 'primary'") != std::string::npos,
           "attribute row carries the verbatim ref + name + kind");

    // The manifest tuple: (pack_namespace, pack_version, id_band, content_hash,
    // schema_version, compatibility_version, mcc_version, built_at).
    const std::vector<std::string> cells = parse_manifest_values(sql);
    report(cells.size() == 8, "manifest row has 8 columns (worldd's read shape)",
           "got " + std::to_string(cells.size()));
    if (cells.size() == 8) {
        report(cells[0] == "core", "pack_namespace = core", cells[0]);
        report(cells[1] == "1.2.3", "pack_version from pack.yaml", cells[1]);
        report(is_lower_hex_64(cells[3]), "content_hash is 64 lowercase-hex (BLAKE3)",
               cells[3]);
        report(cells[4] == "1", "schema_version = 1 (worldd's supported major)",
               cells[4]);
        report(cells[5] == "1", "compatibility_version defaults to 1 (#698)", cells[5]);
        report(cells[6] == "test-1.0.0", "mcc_version stamped", cells[6]);
        report(cells[7] == "2026-07-06 12:00:00", "built_at is the parameterized ts",
               cells[7]);
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

// A two-pack fixture: `alpha` (sorts before core) + `core`, each with one ability
// so both earn an idmap AND emit a content row. Exercises emit-sql's single-pack
// selection (--pack <ns>): the manifest gets one row per pack and each pack's
// ability lands in the `ability` table.
fs::path make_two_pack_fixture() {
    const fs::path root = make_scratch("twopack");
    const fs::path content = root / "content";
    write_file(content / "alpha" / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: alpha\n"
               "name: Alpha Pack\n"
               "version: 0.1.0\n"
               "content_schema_version: 1\n");
    write_file(content / "alpha" / "abilities" / "bolt.ability.yaml",
               "schema: meridian/ability@1\n"
               "id: alpha:ability.bolt\n"
               "name: Bolt\n"
               "school: arcane\n"
               "target: enemy\n"
               "range_m: 30\n"
               "cast:\n  time_ms: 1000\n"
               "cooldown_ms: 5000\n"
               "effects:\n"
               "  - kind: damage\n"
               "    amount: { min: 5, max: 9 }\n");
    write_file(content / "core" / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: core\n"
               "name: Core Pack\n"
               "version: 0.1.0\n"
               "content_schema_version: 1\n");
    write_file(content / "core" / "abilities" / "zap.ability.yaml",
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
    return content.string();
}

// Run emit-sql over a content dir with an explicit --pack selection.
mcc::stages::EmitSqlResult run_emit_select(const std::string& content_dir,
                                           const std::string& select_ns, bool& ok) {
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
    opts.select_namespace = select_ns;
    mcc::stages::EmitSqlResult res = mcc::stages::emit_sql(model, linked, opts, diags);
    ok = diags.ok();
    return res;
}

// --pack makes emit-sql produce a single-pack world DB (design §4): one
// world_manifest row + only that pack's content rows. It must (a) default to the
// multi-pack dump (every pack's row + all content), (b) emit only the named pack
// when --pack is given, and (c) fail on an unknown namespace rather than silently
// emit the wrong (or every) pack.
void test_multipack_selection() {
    std::cout << "[multipack] --pack selects a single-pack world DB\n";
    const std::string content = make_two_pack_fixture();

    bool ok_default = false;
    const auto def = run_emit_select(content, "", ok_default);
    report(ok_default && def.manifest_rows == 2,
           "default emits every pack's world_manifest row (alpha + core)",
           std::to_string(def.manifest_rows));
    report(def.sql.find("'alpha'") != std::string::npos &&
               def.sql.find("'core'") != std::string::npos,
           "default manifest carries both pack rows");
    report(def.sql.find("alpha:ability.bolt") == std::string::npos &&
               def.sql.find("core:ability.zap") == std::string::npos,
           "content refs resolved to numeric ids (no string ids leak)");

    bool ok_core = false;
    const auto core = run_emit_select(content, "core", ok_core);
    report(ok_core && core.manifest_rows == 1,
           "--pack core emits exactly one world_manifest row",
           std::to_string(core.manifest_rows));
    report(core.sql.find("'core'") != std::string::npos &&
               core.sql.find("'alpha'") == std::string::npos,
           "--pack core manifest carries only the core row (alpha dropped)");

    bool ok_alpha = false;
    const auto alpha = run_emit_select(content, "alpha", ok_alpha);
    report(ok_alpha && alpha.manifest_rows == 1 &&
               alpha.sql.find("'alpha'") != std::string::npos &&
               alpha.sql.find("'core'") == std::string::npos,
           "--pack alpha emits only the alpha row even though alpha sorts first");

    // The core and alpha abilities each occupy exactly one `ability` row; a
    // single-pack emit must carry ITS pack's ability and drop the other's. The
    // ability inserts key by numeric id, so count the INSERT INTO ability blocks'
    // row comment to confirm the content was filtered, not just the manifest.
    report(core.content_rows == alpha.content_rows && core.content_rows > 0,
           "each single-pack emit carries its own content rows (symmetric)",
           "core=" + std::to_string(core.content_rows) +
               " alpha=" + std::to_string(alpha.content_rows));
    report(def.content_rows == core.content_rows + alpha.content_rows,
           "multi-pack content_rows == sum of the two single-pack emits",
           "def=" + std::to_string(def.content_rows) + " core=" +
               std::to_string(core.content_rows) + " alpha=" +
               std::to_string(alpha.content_rows));

    bool ok_missing = true;
    const auto missing = run_emit_select(content, "nope", ok_missing);
    report(!ok_missing && !missing.ok,
           "--pack for an absent namespace is an error, not a silent wrong pick");
}

// A fixture exercising the npc@2 visual.appearance branch (assemble-like-a-player):
// a race with roster_id 7 that renders as an appearance_catalog, the catalog itself
// (sex male, non-empty presets hair=3/face=2/skin=5), and an NPC that references the
// catalog via visual.appearance. emit-sql must lower the NPC's visual.appearance to
// the 5 npc_template.visual_appearance_* columns: race roster id 7, sex 0, and the
// first preset id of each list.
fs::path make_appearance_fixture() {
    const fs::path root = make_scratch("appearance");
    const fs::path content = root / "content";
    const fs::path core = content / "core";
    write_file(core / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: core\n"
               "name: Test Pack\n"
               "version: 1.0.0\n"
               "content_schema_version: 1\n");
    write_file(core / "races" / "hero.race.yaml",
               "schema: meridian/race@1\n"
               "id: core:race.hero\n"
               "roster_id: 7\n"
               "name: Hero\n"
               "appearance: core:appearance.hero.male\n");
    write_file(core / "appearance" / "hero_male.appearance.yaml",
               "schema: meridian/appearance_catalog@1\n"
               "id: core:appearance.hero.male\n"
               "race: hero\n"
               "sex: male\n"
               "skeleton: core:art.rig\n"
               "body_model: core:art.body\n"
               "presets:\n"
               "  hair:\n    - { id: 3, model: core:art.hair }\n"
               "  face:\n    - { id: 2, texture: core:art.face }\n"
               "  skin:\n    - { id: 5, palette: core:art.skin }\n");
    write_file(core / "npcs" / "hero.npc.yaml",
               "schema: meridian/npc@2\n"
               "id: core:npc.hero\n"
               "name: Herald\n"
               "level: { min: 1, max: 1 }\n"
               "creature_type: humanoid\n"
               "faction: friendly\n"
               "stats:\n  health: 10\n  damage: { min: 1, max: 2 }\n  attack_speed_ms: 2000\n"
               "visual:\n  appearance: appearance.hero.male\n");
    // A model-only NPC in the SAME emit — its 5 appearance columns must round-trip
    // NULL (proving the branch-A/branch-B split; no behaviour change for model NPCs).
    write_file(core / "npcs" / "grunt.npc.yaml",
               "schema: meridian/npc@2\n"
               "id: core:npc.grunt\n"
               "name: Grunt\n"
               "level: { min: 1, max: 1 }\n"
               "creature_type: beast\n"
               "faction: hostile\n"
               "stats:\n  health: 10\n  damage: { min: 1, max: 2 }\n  attack_speed_ms: 2000\n"
               "visual:\n  model: art.creature.grunt\n");
    // The grunt's model needs an IF-9 asset id so emit_npc resolves visual_model_id.
    write_file(core / "assets" / "grunt.asset.yaml",
               "schema: meridian/asset@1\n"
               "id: core:art.creature.grunt\n"
               "class: creature_model\n"
               "source: assets/art/creature/grunt.glb\n"
               "license: CC-BY-4.0\n"
               "provenance:\n  source_tier: original\n  authors: [test]\n");
    return content.string();
}

// npc@2 visual.appearance -> the 5 npc_template.visual_appearance_* columns, and
// model-only NPCs round-trip them NULL (no behaviour change).
void test_npc_appearance_projection() {
    std::cout << "test_npc_appearance_projection (visual.appearance -> 5 columns)\n";
    const std::string content = make_appearance_fixture();
    bool ok = false;
    const std::string sql = run_emit(content, ok).sql;
    report(ok, "appearance fixture emits with no diagnostics");
    // The npc_template column list must carry the 5 new columns in order.
    report(sql.find("visual_appearance_race_id, visual_appearance_sex, "
                    "visual_appearance_hair, visual_appearance_face, "
                    "visual_appearance_skin") != std::string::npos,
           "npc_template declares the 5 visual_appearance_* columns");
    // Herald (branch B): the row tail after visual_sound_set_id (NULL, no model/
    // scale/sound_set) must be race=7, sex=0, hair=3, face=2, skin=5 — the race
    // roster id + the first preset id of each list.
    const std::size_t herald = sql.find("'Herald'");
    report(herald != std::string::npos, "appearance NPC row present");
    if (herald != std::string::npos) {
        const std::size_t end = sql.find(')', herald);
        const std::string row = sql.substr(herald, end - herald);
        report(row.find(", NULL, NULL, NULL, 7, 0, 3, 2, 5") != std::string::npos,
               "appearance NPC projects race=7,sex=0,hair=3,face=2,skin=5 (model NULL)",
               row);
    }
    // Grunt (branch A): model set, all 5 appearance columns NULL.
    const std::size_t grunt = sql.find("'Grunt'");
    report(grunt != std::string::npos, "model-only NPC row present");
    if (grunt != std::string::npos) {
        const std::size_t end = sql.find(')', grunt);
        const std::string row = sql.substr(grunt, end - grunt);
        // The last 5 cells (visual_appearance_*) must all be NULL. `row` ends at the
        // skin cell (substr excludes the closing paren), so the 5 appearance columns
        // are the row's suffix.
        const std::string tail = ", NULL, NULL, NULL, NULL, NULL";
        const bool ends_null = row.size() >= tail.size() &&
                               row.compare(row.size() - tail.size(), tail.size(), tail) == 0;
        report(ends_null,
               "model-only NPC round-trips the 5 appearance columns as NULL", row);
    }
}

}  // namespace

int main() {
    std::cout << "mcc emit-sql (IF-4 world DB SQL + manifest) unit tests\n\n";
    test_blake3();
    test_emit_wellformed();
    test_determinism();
    test_ref_resolution();
    test_npc_appearance_projection();
    test_multipack_selection();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
