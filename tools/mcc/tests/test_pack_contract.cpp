// tools/mcc/tests/test_pack_contract.cpp — pack-contract tests (story #654):
// the idmap append-only lint (L016), the `mcc diff` breaking-change classifier,
// and the pack content_hash surface. Self-contained (no GoogleTest), matching
// test_link.cpp / test_index.cpp style. Exit 0 = all pass.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "stages/check.h"
#include "stages/content_hash.h"
#include "stages/diagnostics.h"
#include "stages/idmap.h"
#include "stages/pack_contract.h"

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
                          ("mcc_pack_contract_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

// Count L016 diagnostics in a scan of one hand-built idmap.
std::size_t l016_count(const mcc::idmap::IdMap& m) {
    mcc::diag::Diagnostics diags;
    mcc::stages::scan_idmap_append_only(m, "ns/idmap.lock", diags);
    std::size_t n = 0;
    for (const auto& d : diags.items()) {
        if (d.rule == "L016") ++n;
    }
    return n;
}

// ---- Append-only lint (L016) ----------------------------------------------

void test_append_only_lint() {
    std::cout << "test_append_only_lint\n";

    mcc::idmap::IdMap clean;
    clean.namespace_ = "ns";
    clean.map = {{"ns:npc.a", 1}, {"ns:zone.z", 2}};
    report(l016_count(clean) == 0, "clean idmap has no L016");

    mcc::idmap::IdMap collision = clean;
    collision.map["ns:zone.z"] = 1;  // renumbered onto npc.a's slot
    report(l016_count(collision) >= 1, "renumber collision (two ids, one index) -> L016");

    mcc::idmap::IdMap retired_reuse = clean;
    retired_reuse.retired = {{"ns:npc.old", 1}};  // index 1 frozen but still live
    report(l016_count(retired_reuse) >= 1, "reuse of a retired index -> L016");

    mcc::idmap::IdMap dual = clean;
    dual.retired = {{"ns:npc.a", 9}};  // id both live and retired
    report(l016_count(dual) >= 1, "id in both map and retired -> L016");

    mcc::idmap::IdMap zero = clean;
    zero.map["ns:zone.z"] = 0;  // reserved null index
    report(l016_count(zero) >= 1, "reserved index 0 -> L016");
}

// ---- Fixture builder for diff / content-hash -------------------------------

// Write a one-namespace content pack: pack.yaml (optional compatibility_version),
// an npc entity with the given top-level `extra` field lines, and an idmap.lock
// mapping the entity to `npc_index`.
void write_pack(const fs::path& root, int compat, const std::string& npc_extra,
                unsigned npc_index) {
    std::string pack = "schema: meridian/pack@1\nnamespace: mini\nname: Mini\n"
                       "version: 0.1.0\ncontent_schema_version: 1\n"
                       "engine:\n  godot: \"4.6\"\nlicense: Apache-2.0\n";
    if (compat > 0) pack += "compatibility_version: " + std::to_string(compat) + "\n";
    write_file(root / "mini" / "pack.yaml", pack);

    write_file(root / "mini" / "npcs" / "a.npc.yaml",
               "schema: meridian/npc@2\nid: mini:npc.a\nname: A\n" + npc_extra);

    std::ostringstream lock;
    lock << "schema: meridian/idmap@1\nnamespace: mini\nband: 0\nreleased_watermark: 0\n"
         << "map:\n  mini:npc.a: " << npc_index << "\n";
    write_file(root / "mini" / "idmap.lock", lock.str());
}

// ---- `mcc diff` classifier -------------------------------------------------

void test_diff() {
    std::cout << "test_diff\n";
    const fs::path base = make_scratch("diff");

    // Old baseline: one npc, index 1, compat 1.
    const fs::path old_dir = base / "old";
    write_pack(old_dir, 1, "level: 1\n", 1);

    // (a) Additive: a NEW optional field on the same entity -> additive, exit 0.
    {
        const fs::path new_dir = base / "additive";
        write_pack(new_dir, 1, "level: 1\nfaction: friendly\n", 1);
        std::ostringstream out, err;
        const int rc = mcc::stages::diff_packs(old_dir.string(), new_dir.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
        report(rc == 0, "additive diff exits 0", "rc=" + std::to_string(rc));
        report(out.str().find("ADDITIVE") != std::string::npos,
               "additive diff classified ADDITIVE", out.str());
        report(out.str().find("BREAKING") == std::string::npos,
               "additive diff has no BREAKING lines", out.str());
        report(out.str().find("faction") != std::string::npos,
               "additive diff names the new field", out.str());
    }

    // (b) Breaking: a REMOVED field (capability) -> breaking, exit 1, named.
    {
        const fs::path new_dir = base / "removed_field";
        write_pack(new_dir, 2, "faction: friendly\n", 1);  // dropped `level`, bumped compat
        std::ostringstream out, err;
        const int rc = mcc::stages::diff_packs(old_dir.string(), new_dir.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
        report(rc == 1, "removed-field diff exits 1", "rc=" + std::to_string(rc));
        report(out.str().find("BREAKING") != std::string::npos,
               "removed-field diff classified BREAKING", out.str());
        report(out.str().find("mini:npc.a.level") != std::string::npos,
               "removed-field diff names the exact id.field", out.str());
    }

    // (c) Breaking: a RENUMBERED id (idmap index changed) -> breaking, exit 1.
    {
        const fs::path new_dir = base / "renumbered";
        write_pack(new_dir, 2, "level: 1\n", 5);  // same entity, index 1 -> 5
        std::ostringstream out, err;
        const int rc = mcc::stages::diff_packs(old_dir.string(), new_dir.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
        report(rc == 1, "renumbered-id diff exits 1", "rc=" + std::to_string(rc));
        report(out.str().find("renumbered id: mini:npc.a") != std::string::npos,
               "renumbered diff names the id + indices", out.str());
    }

    // (d) Breaking WITHOUT a compatibility_version bump -> the boot-gate flag.
    {
        const fs::path new_dir = base / "no_bump";
        write_pack(new_dir, 1, "faction: friendly\n", 1);  // dropped level, compat still 1
        std::ostringstream out, err;
        const int rc = mcc::stages::diff_packs(old_dir.string(), new_dir.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
        report(rc == 1, "no-bump breaking diff exits 1", "rc=" + std::to_string(rc));
        report(out.str().find("compatibility_version not bumped") != std::string::npos,
               "no-bump diff flags the compatibility_version gate", out.str());
    }

    // (e) Identical packs -> additive (no changes), exit 0.
    {
        const fs::path new_dir = base / "identical";
        write_pack(new_dir, 1, "level: 1\n", 1);
        std::ostringstream out, err;
        const int rc = mcc::stages::diff_packs(old_dir.string(), new_dir.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
        report(rc == 0, "identical diff exits 0", "rc=" + std::to_string(rc));
        report(out.str().find("BREAKING") == std::string::npos,
               "identical diff has no BREAKING", out.str());
    }

    // (f) A missing content root is a usage error (exit 2).
    {
        std::ostringstream out, err;
        const int rc = mcc::stages::diff_packs((base / "nope").string(), old_dir.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
        report(rc == 2, "missing content root -> exit 2", "rc=" + std::to_string(rc));
    }
}

// ---- content_hash stability ------------------------------------------------

std::string hash_of(const std::string& dir) {
    std::ostringstream out, err;
    mcc::stages::content_hash_report(dir, /*as_json=*/false, out, err);
    return out.str();
}

void test_content_hash_stability() {
    std::cout << "test_content_hash_stability\n";
    const fs::path base = make_scratch("hash");

    const fs::path a = base / "a";
    write_pack(a, 1, "level: 1\n", 1);

    const std::string h1 = hash_of(a.string());
    const std::string h2 = hash_of(a.string());
    report(!h1.empty(), "content-hash produces output", h1);
    report(h1 == h2, "content-hash is stable across runs on unchanged content");

    // A content change flips the digest.
    const fs::path b = base / "b";
    write_pack(b, 1, "level: 2\n", 1);
    report(hash_of(b.string()) != h1, "content-hash changes when content changes");

    // The new pack-manifest fields flow into the digest (compatibility_version).
    const fs::path c = base / "c";
    write_pack(c, 7, "level: 1\n", 1);
    report(hash_of(c.string()) != h1,
           "content-hash changes when compatibility_version changes");
}

}  // namespace

int main() {
    test_append_only_lint();
    test_diff();
    test_content_hash_stability();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
