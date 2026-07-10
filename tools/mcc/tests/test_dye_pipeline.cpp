// tools/mcc/tests/test_dye_pipeline.cpp — pipeline coverage for the CLIENT-side
// content types added by contract ① (dye + appearance catalogs). Self-contained
// (no GoogleTest), matching test_emit_pck.cpp / test_emit_sql.cpp style.
//
// Contract (final-review C1, spec §5.1/§6/§8):
//   * a `.dye.yaml` / `.appearance.yaml` file CLASSIFIES (discover) instead of
//     hard-failing L001 as FileKind::Unknown;
//   * it VALIDATES its envelope — `dye` → `meridian/dye@1`, and the irregular
//     `appearance` → `meridian/appearance_catalog@1` (the ENVELOPE_TYPE_NAMES
//     map that tools/validate_content.py already carries);
//   * it flows into the CLIENT pack contents (emit-pck) like every other
//     entity — visuals ship to the client;
//   * it produces NO world.sql rows — the server never reads visuals (spec §8:
//     "server never sees visuals"), so dye/appearance must not reach IF-4.
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

// A fixture pack carrying one ordinary server-side entity (an ability, which
// DOES emit world.sql rows) alongside the two client-only types (dye +
// appearance). The ability is the contrast: it proves emit-sql is working, so
// the dye/appearance ABSENCE from the SQL is a real "skipped", not a dead emit.
fs::path make_fixture() {
    static int counter = 0;
    const fs::path base = fs::temp_directory_path() /
                          ("mcc_dye_test_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path content = base / "content";
    const fs::path core = content / "core";
    write_file(core / "pack.yaml",
               "schema: meridian/pack@1\n"
               "namespace: core\n"
               "name: Test Pack\n"
               "version: 1.0.0\n"
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
    write_file(core / "dyes" / "bone.dye.yaml",
               "schema: meridian/dye@1\n"
               "id: core:dye.bone\n"
               "name: Bone\n"
               "color: \"#d9cfc0\"\n"
               "rarity: common\n");
    // The `appearance` file type has an IRREGULAR envelope name: the file suffix
    // is `.appearance.yaml` but the envelope word is `appearance_catalog`.
    write_file(core / "appearance" / "human_male.appearance.yaml",
               "schema: meridian/appearance_catalog@1\n"
               "id: core:appearance.human_male\n"
               "race: human\n"
               "sex: male\n"
               "presets: []\n");
    return content.string();
}

// -- (1) classification -------------------------------------------------------
void test_classification() {
    std::cout << "test_classification (dye/appearance discover as Content)\n";

    std::string dye_type, app_type;
    const mcc::model::FileKind dye_kind =
        mcc::stages::classify("bone.dye.yaml", dye_type);
    const mcc::model::FileKind app_kind =
        mcc::stages::classify("human_male.appearance.yaml", app_type);

    report(dye_kind == mcc::model::FileKind::Content && dye_type == "dye",
           "bone.dye.yaml classifies as Content type 'dye'",
           "kind int=" + std::to_string(static_cast<int>(dye_kind)) +
               " type='" + dye_type + "'");
    report(app_kind == mcc::model::FileKind::Content && app_type == "appearance",
           "human_male.appearance.yaml classifies as Content type 'appearance'",
           "kind int=" + std::to_string(static_cast<int>(app_kind)) +
               " type='" + app_type + "'");
    report(mcc::stages::is_content_type("dye") &&
               mcc::stages::is_content_type("appearance"),
           "is_content_type recognises dye + appearance");
}

// -- (2) envelope validation --------------------------------------------------
void test_envelope_validates() {
    std::cout << "test_envelope_validates (no L001 for dye@1 / appearance_catalog@1)\n";

    const std::string content_dir = make_fixture();
    mcc::model::ContentModel model;
    mcc::stages::discover(content_dir, model);
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);

    report(diags.ok(),
           "full validate() is clean (dye@1 + appearance_catalog@1 accepted)",
           "error_count=" + std::to_string(diags.error_count()));
}

// -- (3) pck contents + (4) no world.sql -------------------------------------
void test_pck_carries_dye_but_sql_skips_it() {
    std::cout << "test_pck_carries_dye_but_sql_skips_it (client pck yes, IF-4 no)\n";

    const std::string content_dir = make_fixture();

    // --- shared front half: discover -> parse -> validate -> link (allocate). --
    mcc::model::ContentModel model;
    mcc::stages::discover(content_dir, model);
    mcc::diag::Diagnostics diags;
    mcc::stages::parse(model, diags);
    mcc::stages::validate(model, diags);
    const mcc::stages::LinkResult linked =
        mcc::stages::link(model, content_dir, /*allocate=*/true, diags,
                          /*emit_dangling=*/false);

    mcc::stages::EmitPckOptions pck_opts;
    pck_opts.mcc_version = "test-1.0.0";
    pck_opts.built_at = "2026-07-06 12:00:00";
    const mcc::stages::EmitPckResult pck =
        mcc::stages::emit_pck(model, linked, pck_opts, diags);

    mcc::stages::EmitSqlOptions sql_opts;
    sql_opts.mcc_version = "test-1.0.0";
    sql_opts.built_at = "2026-07-06 12:00:00";
    const mcc::stages::EmitSqlResult sql =
        mcc::stages::emit_sql(model, linked, sql_opts, diags);

    report(diags.ok(), "emit-pck + emit-sql ran clean");

    // (3) dye + appearance are present in the client pack contents.
    bool has_dye = false, has_appearance = false, has_ability = false;
    for (const auto& e : pck.entries) {
        if (e.type == "dye" && e.id == "core:dye.bone") has_dye = true;
        if (e.type == "appearance" && e.id == "core:appearance.human_male")
            has_appearance = true;
        if (e.type == "ability") has_ability = true;
    }
    report(has_dye, "dye lands in client pack contents (emit-pck)");
    report(has_appearance, "appearance catalog lands in client pack contents (emit-pck)");
    report(has_ability, "sanity: the ability also lands in pck (contrast entity)");

    // (4) the SERVER world.sql carries the ability but NOT the client visuals.
    report(sql.sql.find("INSERT INTO ability") != std::string::npos,
           "sanity: emit-sql DID emit the ability rows (emit path is live)");
    report(sql.sql.find("INSERT INTO dye") == std::string::npos,
           "no world.sql table/insert for dye (server never reads visuals)");
    report(sql.sql.find("INSERT INTO appearance") == std::string::npos,
           "no world.sql table/insert for appearance (server never reads visuals)");
    report(sql.sql.find("core:dye.bone") == std::string::npos,
           "the dye id never leaks into world.sql");
}

}  // namespace

int main() {
    std::cout << "mcc dye/appearance pipeline (contract ① client-content) unit tests\n\n";
    test_classification();
    test_envelope_validates();
    test_pck_carries_dye_but_sql_skips_it();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
