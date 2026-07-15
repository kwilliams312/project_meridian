// tools/mcc/src/main.cpp — `mcc` CLI dispatcher (v0 skeleton).
//
// Meridian Content Compiler (TLS-01): one C++20 binary, two outputs — world-DB
// SQL (IF-4) for the server and client `.pck` packs (IF-5). This file is the CLI
// surface only: hand-rolled argument parsing, one handler per subcommand, each
// currently a stub that prints its role (per Tools SAD §2) and exits 0. Real
// compilation logic (typed content model, schema+lint validation, IF-9 idmap,
// Recast bake, deterministic emit) arrives in later M0 tasks.
//
// CLI surface (v1) — Tools SAD §2.8 / Tools PRD §2.2:
//   mcc build [--full|--watch] | check | fmt | diff <A> <B> | pack
//       | install <pack> | uninstall <pack> | migrate | idmap verify|reassign
//   mcc --version | --help

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "stages/check.h"
#include "stages/chunk_emit.h"
#include "stages/content_hash.h"
#include "stages/format.h"
#include "stages/index.h"
#include "stages/pack_contract.h"
#include "stages/stages.h"

#ifndef MCC_VERSION
#define MCC_VERSION "0.0.0"
#endif

namespace {

constexpr std::string_view kProg = "mcc";

// A subcommand: name, its one-line --help description, and its handler.
struct Command {
    std::string_view name;
    std::string_view summary;
    int (*run)(const std::vector<std::string>& args);
};

// Emit a stub notice for a stage of the pipeline this subcommand would drive.
void emit_stage(mcc::stages::Stage s) { mcc::stages::run(s, std::cout); }

// Default content root when none is given on the command line. `mcc` is run
// from the repo root in CI and by editors, so ./content is the convention.
constexpr std::string_view kDefaultContentDir = "./content";

// The reproducible default build timestamp stamped into world_manifest.built_at
// when --built-at is not given. NOT the wall clock — a fixed epoch keeps
// double-build output byte-identical (Tools SAD §5 determinism). A real nightly
// build passes its own value via --built-at.
constexpr std::string_view kDefaultBuiltAt = "1970-01-01 00:00:00";

// Shared parse of the check/build diagnostics flags: `--diag-format=text|json`,
// an optional positional content directory, and (check only) `--file <path>`
// for single-file / validation-as-you-type mode. Returns false on a bad flag
// (with a message on stderr); on success fills `content_dir`, `format`, and
// `single_file` (empty unless `--file` was given). `--file` accepts both
// `--file=<path>` and `--file <path>` forms.
bool parse_check_flags(std::string_view prog_sub, const std::vector<std::string>& args,
                       std::string& content_dir, mcc::stages::DiagFormat& format,
                       std::string& single_file) {
    content_dir = std::string(kDefaultContentDir);
    format = mcc::stages::DiagFormat::Text;
    single_file.clear();
    bool have_dir = false;
    bool expect_file = false;  // saw a bare "--file"; next token is its value
    for (const auto& a : args) {
        if (expect_file) {
            single_file = a;
            expect_file = false;
        } else if (a == "--diag-format=json") {
            format = mcc::stages::DiagFormat::Json;
        } else if (a == "--diag-format=text") {
            format = mcc::stages::DiagFormat::Text;
        } else if (a.rfind("--diag-format=", 0) == 0) {
            std::cerr << kProg << " " << prog_sub << ": unknown --diag-format value '"
                      << a.substr(std::strlen("--diag-format=")) << "' (expected text | json)\n";
            return false;
        } else if (a == "--file") {
            expect_file = true;
        } else if (a.rfind("--file=", 0) == 0) {
            single_file = a.substr(std::strlen("--file="));
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << kProg << " " << prog_sub << ": unknown flag '" << a << "'\n";
            return false;
        } else if (!have_dir) {
            content_dir = a;
            have_dir = true;
        } else {
            std::cerr << kProg << " " << prog_sub << ": unexpected extra argument '" << a << "'\n";
            return false;
        }
    }
    if (expect_file) {
        std::cerr << kProg << " " << prog_sub << ": --file requires a path argument\n";
        return false;
    }
    return true;
}

// Convenience overload for callers that never take `--file` (e.g. build).
bool parse_check_flags(std::string_view prog_sub, const std::vector<std::string>& args,
                       std::string& content_dir, mcc::stages::DiagFormat& format) {
    std::string ignored_single_file;
    if (!parse_check_flags(prog_sub, args, content_dir, format, ignored_single_file))
        return false;
    if (!ignored_single_file.empty()) {
        std::cerr << kProg << " " << prog_sub << ": --file is not supported here\n";
        return false;
    }
    return true;
}

// ---- Subcommand handlers ---------------------------------------------------
// Each prints "stub: <role per the SAD>" and returns 0. Flags that the real
// implementation will honor are parsed now so the CLI surface is stable.

int cmd_build(const std::vector<std::string>& args) {
    bool full = false;
    bool watch = false;
    // Editor-invoked builds allocate ids by default (write idmap.lock); CI opts
    // out with --no-allocate-ids to run link read-only and fail on L015 drift
    // (Tools SAD §2.3). --allocate-ids is accepted explicitly for symmetry.
    bool allocate_ids = true;
    std::vector<std::string> check_args;  // non-build flags forwarded to check
    for (const auto& a : args) {
        if (a == "--full") full = true;
        else if (a == "--watch") watch = true;
        else if (a == "--allocate-ids") allocate_ids = true;
        else if (a == "--no-allocate-ids") allocate_ids = false;
        else check_args.push_back(a);  // e.g. --diag-format=json, content dir
    }

    // build runs discover/parse/validate then the real link stage (Tools SAD
    // §2.2-2.4: validate gates the DAG; link resolves refs + allocates IF-9 ids).
    // A failing front half aborts the build — nothing downstream is emitted from
    // bad content.
    std::string content_dir;
    mcc::stages::DiagFormat format;
    if (!parse_check_flags("build", check_args, content_dir, format)) return 2;
    const int link_rc = mcc::stages::link_content(content_dir, format, allocate_ids,
                                                  /*report=*/false, std::cout, std::cerr);
    if (link_rc != 0) {
        std::cerr << kProg << " build: check/link failed — aborting build\n";
        return link_rc;
    }

    std::cout << "build — bake + emit (Tools SAD §2), producing IF-4 SQL + IF-5 .pck\n";
    std::cout << "  mode: " << (full ? "full rebuild" : "incremental")
              << (watch ? ", watching for changes" : "") << '\n';
    // discover/parse/validate/link ran above; bake is still a stub. emit-sql
    // (#120) is real: after link allocated the ids, emit the IF-4 world DB SQL
    // to <content>/../build/world.sql (a stable path next to the content root).
    emit_stage(mcc::stages::Stage::Bake);
    const std::string world_sql = "build/world.sql";
    const int emit_rc = mcc::stages::emit_sql_content(
        content_dir, world_sql, MCC_VERSION, std::string(kDefaultBuiltAt), format,
        std::cout, std::cerr);
    if (emit_rc != 0) {
        std::cerr << kProg << " build: emit-sql failed — aborting build\n";
        return emit_rc;
    }
    std::cout << "  emit-sql: wrote IF-4 world DB SQL -> " << world_sql << '\n';

    // emit-pck (#121): after link allocated the ids and emit-sql produced the
    // IF-4 SQL, assemble the IF-5 client pack (pack.manifest.json + M0 pack) under
    // build/pck/. Its content_hash is byte-identical to the world_manifest hash
    // above (the three-way tie, SAD §2.6). A failing emit aborts the build.
    const std::string pck_dir = "build/pck";
    const int pck_rc = mcc::stages::emit_pck_content(
        content_dir, pck_dir, MCC_VERSION, std::string(kDefaultBuiltAt),
        /*godot_version=*/"", format, std::cout, std::cerr);
    if (pck_rc != 0) {
        std::cerr << kProg << " build: emit-pck failed — aborting build\n";
        return pck_rc;
    }
    std::cout << "  emit-pck: wrote IF-5 client pack -> " << pck_dir
              << "/meridian/<ns>/pack.manifest.json\n";
    return 0;
}

// mcc link [dir] [--diag-format=...] [--allocate-ids|--no-allocate-ids] [--report]
//   Resolve the reference graph, build backlinks, and allocate IF-9 numeric ids
//   (Tools SAD §2.3/§2.4). --report prints the allocated idmap per namespace.
//   Default allocates ids (writes idmap.lock); --no-allocate-ids runs read-only
//   (the CI contract, fails on L015 idmap drift).
int cmd_link(const std::vector<std::string>& args) {
    bool allocate_ids = true;
    bool report = false;
    std::vector<std::string> check_args;
    for (const auto& a : args) {
        if (a == "--allocate-ids") allocate_ids = true;
        else if (a == "--no-allocate-ids") allocate_ids = false;
        else if (a == "--report") report = true;
        else check_args.push_back(a);
    }
    std::string content_dir;
    mcc::stages::DiagFormat format;
    if (!parse_check_flags("link", check_args, content_dir, format)) return 2;
    return mcc::stages::link_content(content_dir, format, allocate_ids, report,
                                     std::cout, std::cerr);
}

// Shared flag parse for the index-family commands (index/pickable/refs): an
// optional positional content directory plus `--json`. `pos_wanted` is the count
// of NON-dir positional arguments the command requires (the id/type argument):
// pickable/refs want 1, index wants 0. Fills `positional` (in order) and
// `content_dir` (the LAST positional beyond `pos_wanted`, or the default).
// Returns false on an unknown flag or wrong positional count (message on stderr).
bool parse_index_flags(std::string_view prog_sub, const std::vector<std::string>& args,
                       std::size_t pos_wanted, std::vector<std::string>& positional,
                       std::string& content_dir, bool& as_json) {
    content_dir = std::string(kDefaultContentDir);
    as_json = false;
    positional.clear();
    std::vector<std::string> pos;  // all non-flag args in order
    for (const auto& a : args) {
        if (a == "--json") {
            as_json = true;
        } else if (a == "--diag-format=json") {
            // Accept the check/build spelling too: index JSON is on stdout.
            as_json = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << kProg << " " << prog_sub << ": unknown flag '" << a << "'\n";
            return false;
        } else {
            pos.push_back(a);
        }
    }
    // The required argument(s) come first; an OPTIONAL trailing positional is the
    // content dir. So valid arities are pos_wanted or pos_wanted+1.
    if (pos.size() < pos_wanted) {
        std::cerr << kProg << " " << prog_sub << ": missing required argument\n";
        return false;
    }
    if (pos.size() > pos_wanted + 1) {
        std::cerr << kProg << " " << prog_sub << ": unexpected extra argument '"
                  << pos[pos_wanted + 1] << "'\n";
        return false;
    }
    for (std::size_t i = 0; i < pos_wanted; ++i) positional.push_back(pos[i]);
    if (pos.size() == pos_wanted + 1) content_dir = pos[pos_wanted];
    return true;
}

// mcc index [dir] [--json]
//   Surface the link stage's ID index (Tools SAD §2.3/§6): all content+asset ids
//   grouped by type, each with its IF-9 numeric id + source file. `--json` emits
//   the stable editor-consumable contract (meridian/id-index@1) on stdout.
int cmd_index(const std::vector<std::string>& args) {
    std::vector<std::string> pos;
    std::string content_dir;
    bool as_json = false;
    if (!parse_index_flags("index", args, 0, pos, content_dir, as_json)) return 2;
    return mcc::stages::index_content(content_dir, as_json, std::cout, std::cerr);
}

// mcc pickable <ref-type> [dir] [--json]
//   Typed reference picker (Tools SAD §2.3/§6, TLS-03): for a ref field of type
//   <ref-type> (npc|item|quest|ability|loot|vendor|zone or art|mus|sfx|amb; the
//   schema `*Ref` $defs, "itemRef" or "item" both accepted), list the VALID
//   target ids — the candidates a picker would show. `--json` emits
//   meridian/pickable@1 on stdout.
int cmd_pickable(const std::vector<std::string>& args) {
    std::vector<std::string> pos;
    std::string content_dir;
    bool as_json = false;
    if (!parse_index_flags("pickable", args, 1, pos, content_dir, as_json)) return 2;
    return mcc::stages::pickable_content(content_dir, pos[0], as_json, std::cout, std::cerr);
}

// mcc refs <id> [dir] [--json]
//   Backlinks / find-usages (Tools SAD §2.3/§6): for a content/asset id, list who
//   references it (the reverse graph #119 built), each with the referencing field
//   path. Empty for an unreferenced id. `--json` emits meridian/backlinks@1.
int cmd_refs(const std::vector<std::string>& args) {
    std::vector<std::string> pos;
    std::string content_dir;
    bool as_json = false;
    if (!parse_index_flags("refs", args, 1, pos, content_dir, as_json)) return 2;
    return mcc::stages::refs_content(content_dir, pos[0], as_json, std::cout, std::cerr);
}

// mcc emit-sql [dir] [--out <file>] [--built-at "<ts>"] [--diag-format=...]
//   Emit the world DB DML (IF-4): content-table inserts + the world_manifest row
//   worldd reads at boot (Tools SAD §2.6). Consumes the existing idmap.lock
//   read-only (run `mcc link`/`mcc build --allocate-ids` first to allocate ids).
//   Without --out the SQL goes to stdout (diagnostics to stderr); with --out the
//   SQL is written to the file (diagnostics to stdout).
int cmd_emit_sql(const std::vector<std::string>& args) {
    std::string out_file;
    std::string built_at(kDefaultBuiltAt);
    std::vector<std::string> check_args;
    bool expect_out = false;
    bool expect_built = false;
    for (const auto& a : args) {
        if (expect_out) { out_file = a; expect_out = false; }
        else if (expect_built) { built_at = a; expect_built = false; }
        else if (a == "--out") expect_out = true;
        else if (a.rfind("--out=", 0) == 0) out_file = a.substr(std::strlen("--out="));
        else if (a == "--built-at") expect_built = true;
        else if (a.rfind("--built-at=", 0) == 0) built_at = a.substr(std::strlen("--built-at="));
        else check_args.push_back(a);
    }
    if (expect_out) {
        std::cerr << kProg << " emit-sql: --out requires a path argument\n";
        return 2;
    }
    if (expect_built) {
        std::cerr << kProg << " emit-sql: --built-at requires a timestamp argument\n";
        return 2;
    }
    std::string content_dir;
    mcc::stages::DiagFormat format;
    if (!parse_check_flags("emit-sql", check_args, content_dir, format)) return 2;
    return mcc::stages::emit_sql_content(content_dir, out_file, MCC_VERSION, built_at,
                                         format, std::cout, std::cerr);
}

// mcc emit-pck [dir] [--out <dir>] [--built-at "<ts>"] [--godot-version <v>]
//              [--pack <ns>] [--diag-format=...]
//   Assemble the IF-5 client pack (Tools SAD §2.7): pack.manifest.json + an M0
//   directory-manifest pack. Consumes the existing idmap.lock read-only (run
//   `mcc link` / `mcc build --allocate-ids` first). Without --out, the manifest
//   goes to stdout (diagnostics to stderr); with --out <dir> the pack is written
//   under <dir>/meridian/<ns>/. The manifest content_hash equals emit-sql's
//   world_manifest content_hash (the three-way tie, SAD §2.6). emit-pck is
//   single-pack at M0: `--pack <ns>` selects which pack to emit when the tree
//   holds several (default: the first pack sorted by namespace).
int cmd_emit_pck(const std::vector<std::string>& args) {
    std::string out_dir;
    std::string built_at(kDefaultBuiltAt);
    std::string godot_version;
    std::string select_ns;
    std::vector<std::string> check_args;
    bool expect_out = false;
    bool expect_built = false;
    bool expect_godot = false;
    bool expect_pack = false;
    for (const auto& a : args) {
        if (expect_out) { out_dir = a; expect_out = false; }
        else if (expect_built) { built_at = a; expect_built = false; }
        else if (expect_godot) { godot_version = a; expect_godot = false; }
        else if (expect_pack) { select_ns = a; expect_pack = false; }
        else if (a == "--out") expect_out = true;
        else if (a.rfind("--out=", 0) == 0) out_dir = a.substr(std::strlen("--out="));
        else if (a == "--built-at") expect_built = true;
        else if (a.rfind("--built-at=", 0) == 0) built_at = a.substr(std::strlen("--built-at="));
        else if (a == "--godot-version") expect_godot = true;
        else if (a.rfind("--godot-version=", 0) == 0) godot_version = a.substr(std::strlen("--godot-version="));
        else if (a == "--pack") expect_pack = true;
        else if (a.rfind("--pack=", 0) == 0) select_ns = a.substr(std::strlen("--pack="));
        else check_args.push_back(a);
    }
    if (expect_out) {
        std::cerr << kProg << " emit-pck: --out requires a directory argument\n";
        return 2;
    }
    if (expect_built) {
        std::cerr << kProg << " emit-pck: --built-at requires a timestamp argument\n";
        return 2;
    }
    if (expect_godot) {
        std::cerr << kProg << " emit-pck: --godot-version requires a version argument\n";
        return 2;
    }
    if (expect_pack) {
        std::cerr << kProg << " emit-pck: --pack requires a namespace argument\n";
        return 2;
    }
    std::string content_dir;
    mcc::stages::DiagFormat format;
    if (!parse_check_flags("emit-pck", check_args, content_dir, format)) return 2;
    return mcc::stages::emit_pck_content(content_dir, out_dir, MCC_VERSION, built_at,
                                         godot_version, format, std::cout, std::cerr,
                                         select_ns);
}

// mcc chunk-emit [--zone <id>] [--grid <N>] [--out <dir>] [--origin-x <m>]
//                [--origin-z <m>] [--built-at "<ts>"] [--godot-version <v>]
//                [--diag-format=...]
//   Emit a procedural N×N chunk fixture pack for a zone (Tools SAD §3, IF-6): the
//   v0 slice of the real mcc chunk stage in fixture mode (#553). Goes through the
//   real chunk.fbs `ServerChunk` schema + chunk-manifest.schema.yaml + real BLAKE3
//   so the pack is byte-shaped like production. Without --out the IF-6 manifest
//   goes to stdout; with --out <dir> the whole fixture (manifest, IF-8 asset
//   table, IF-5 pack, per-chunk .chunk.bin/.tscn/.proxy.tscn) lands under
//   <dir>/meridian/<ns>/chunks/<zone>/. Deliberately non-flat heightfields so
//   downstream flat-vs-sloped bugs are catchable.
int cmd_chunk_emit(const std::vector<std::string>& args) {
    mcc::stages::ChunkEmitOptions opts;
    std::string out_dir;
    mcc::stages::DiagFormat format = mcc::stages::DiagFormat::Text;
    // Small typed-flag parser: each `want_*` remembers a pending value token.
    enum class Want { None, Zone, Grid, Out, OriginX, OriginZ, BuiltAt, Godot };
    Want want = Want::None;
    auto take_double = [](const std::string& s, double& dst) -> bool {
        try { dst = std::stod(s); return true; } catch (...) { return false; }
    };
    auto take_int = [](const std::string& s, int& dst) -> bool {
        try { dst = std::stoi(s); return true; } catch (...) { return false; }
    };
    for (const auto& a : args) {
        if (want != Want::None) {
            switch (want) {
                case Want::Zone: opts.zone = a; break;
                case Want::Grid:
                    if (!take_int(a, opts.grid)) {
                        std::cerr << kProg << " chunk-emit: --grid needs an integer\n";
                        return 2;
                    }
                    break;
                case Want::Out: out_dir = a; break;
                case Want::OriginX:
                    if (!take_double(a, opts.origin_x)) {
                        std::cerr << kProg << " chunk-emit: --origin-x needs a number\n";
                        return 2;
                    }
                    break;
                case Want::OriginZ:
                    if (!take_double(a, opts.origin_z)) {
                        std::cerr << kProg << " chunk-emit: --origin-z needs a number\n";
                        return 2;
                    }
                    break;
                case Want::BuiltAt: opts.built_at = a; break;
                case Want::Godot: opts.godot_version = a; break;
                case Want::None: break;
            }
            want = Want::None;
            continue;
        }
        if (a == "--zone") want = Want::Zone;
        else if (a.rfind("--zone=", 0) == 0) opts.zone = a.substr(std::strlen("--zone="));
        else if (a == "--grid") want = Want::Grid;
        else if (a.rfind("--grid=", 0) == 0) {
            if (!take_int(a.substr(std::strlen("--grid=")), opts.grid)) {
                std::cerr << kProg << " chunk-emit: --grid needs an integer\n";
                return 2;
            }
        } else if (a == "--out") want = Want::Out;
        else if (a.rfind("--out=", 0) == 0) out_dir = a.substr(std::strlen("--out="));
        else if (a == "--origin-x") want = Want::OriginX;
        else if (a.rfind("--origin-x=", 0) == 0) {
            if (!take_double(a.substr(std::strlen("--origin-x=")), opts.origin_x)) {
                std::cerr << kProg << " chunk-emit: --origin-x needs a number\n";
                return 2;
            }
        } else if (a == "--origin-z") want = Want::OriginZ;
        else if (a.rfind("--origin-z=", 0) == 0) {
            if (!take_double(a.substr(std::strlen("--origin-z=")), opts.origin_z)) {
                std::cerr << kProg << " chunk-emit: --origin-z needs a number\n";
                return 2;
            }
        } else if (a == "--built-at") want = Want::BuiltAt;
        else if (a.rfind("--built-at=", 0) == 0) opts.built_at = a.substr(std::strlen("--built-at="));
        else if (a == "--godot-version") want = Want::Godot;
        else if (a.rfind("--godot-version=", 0) == 0) opts.godot_version = a.substr(std::strlen("--godot-version="));
        else if (a == "--diag-format=json") format = mcc::stages::DiagFormat::Json;
        else if (a == "--diag-format=text") format = mcc::stages::DiagFormat::Text;
        else {
            std::cerr << kProg << " chunk-emit: unknown flag '" << a << "'\n";
            return 2;
        }
    }
    if (want != Want::None) {
        std::cerr << kProg << " chunk-emit: a flag is missing its value argument\n";
        return 2;
    }
    opts.mcc_version = MCC_VERSION;
    return mcc::stages::chunk_emit_run(opts, out_dir, format, std::cout, std::cerr);
}

int cmd_check(const std::vector<std::string>& args) {
    // --watch is stripped here (single-file only); the rest go to parse_check_flags.
    bool watch = false;
    std::vector<std::string> rest;
    for (const auto& a : args) {
        if (a == "--watch") watch = true;
        else rest.push_back(a);
    }

    std::string content_dir;
    std::string single_file;
    mcc::stages::DiagFormat format;
    if (!parse_check_flags("check", rest, content_dir, format, single_file)) return 2;

    if (watch && single_file.empty()) {
        std::cerr << kProg << " check: --watch requires --file <path>"
                     " (single-file validation-as-you-type)\n";
        return 2;
    }

    // --file <path> selects the fast single-file / validation-as-you-type path
    // (Tools SAD §6.3): validate one file in isolation, cross-file refs deferred.
    // The positional [dir] (default ./content) is used only as root context to
    // make the diagnostic path root-relative, matching a full check's paths.
    if (!single_file.empty()) {
        if (watch) {
            // Runs until the process is signalled (Ctrl-C); streams each result.
            return mcc::stages::watch_file(single_file, content_dir, format, std::cout,
                                           std::cerr);
        }
        return mcc::stages::check_file(single_file, content_dir, format, std::cout, std::cerr);
    }
    return mcc::stages::check(content_dir, format, std::cout, std::cerr);
}

// mcc fmt [--check] [path] — canonically re-serialize /content YAML.
//   --check : do not write; exit non-zero if any file is not canonical (CI /
//             pre-commit). Otherwise files are rewritten in place when they
//             differ. `path` is a single file or a directory (default ./content).
int cmd_fmt(const std::vector<std::string>& args) {
    bool check_only = false;
    std::string path;
    bool have_path = false;
    for (const auto& a : args) {
        if (a == "--check") {
            check_only = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << kProg << " fmt: unknown flag '" << a << "'\n";
            return 2;
        } else if (!have_path) {
            path = a;
            have_path = true;
        } else {
            std::cerr << kProg << " fmt: unexpected extra argument '" << a << "'\n";
            return 2;
        }
    }
    if (!have_path) path = std::string(kDefaultContentDir);
    return mcc::stages::fmt(path, check_only, std::cout, std::cerr);
}

// mcc diff <old-pack> <new-pack> [--json]
//   Classify every change between two content packs (each a content root dir) as
//   ADDITIVE (new ids, new optional fields) or BREAKING (removed/renumbered id,
//   removed capability, changed id type) — spec §3. Breaking → non-zero exit +
//   an actionable report naming the exact ids/fields; the report also flags a
//   breaking diff that failed to bump compatibility_version (boot-gate contract).
int cmd_diff(const std::vector<std::string>& args) {
    bool as_json = false;
    std::vector<std::string> pos;
    for (const auto& a : args) {
        if (a == "--json" || a == "--diag-format=json") as_json = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << kProg << " diff: unknown flag '" << a << "'\n";
            return 2;
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() != 2) {
        std::cerr << kProg << " diff: expected two packs: "
                  << "diff <old-pack> <new-pack> [--json]\n";
        return 2;
    }
    return mcc::stages::diff_packs(
        pos[0], pos[1], as_json ? mcc::stages::DiagFormat::Json : mcc::stages::DiagFormat::Text,
        std::cout, std::cerr);
}

// mcc content-hash [dir] [--json]
//   Print the per-pack content_hash (the pack-level BLAKE3 over the canonicalized
//   content set, spec §3) — the same digest emit-sql/emit-pck stamp into their
//   manifests (three-way tie). Stable across runs; changes iff content changes.
int cmd_content_hash(const std::vector<std::string>& args) {
    bool as_json = false;
    std::string content_dir(kDefaultContentDir);
    bool have_dir = false;
    for (const auto& a : args) {
        if (a == "--json" || a == "--diag-format=json") as_json = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << kProg << " content-hash: unknown flag '" << a << "'\n";
            return 2;
        } else if (!have_dir) {
            content_dir = a;
            have_dir = true;
        } else {
            std::cerr << kProg << " content-hash: unexpected extra argument '" << a << "'\n";
            return 2;
        }
    }
    return mcc::stages::content_hash_report(content_dir, as_json, std::cout, std::cerr);
}

int cmd_pack(const std::vector<std::string>&) {
    std::cout << "stub: pack — compile + full-ruleset validate, then emit a"
                 " signed .mcpack + content hash (TLS-08, Tools PRD §7)\n";
    return 0;
}

int cmd_install(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << kProg << " install: expected a pack: install <pack>\n";
        return 2;
    }
    std::cout << "stub: install — apply pack '" << args[0]
              << "' into a realm: SQL into its idmap-partitioned band, register"
                 " in the realm manifest (Tools PRD §7)\n";
    return 0;
}

int cmd_uninstall(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << kProg << " uninstall: expected a pack: uninstall <pack>\n";
        return 2;
    }
    std::cout << "stub: uninstall — reverse an install of pack '" << args[0]
              << "', dropping its pack-namespaced rows (Tools PRD §7)\n";
    return 0;
}

int cmd_migrate(const std::vector<std::string>&) {
    std::cout << "stub: migrate — upgrade a pack's YAML source (not its binary)"
                 " to newer content schemas (Tools SAD §2.1, Tools PRD §7)\n";
    return 0;
}

int cmd_idmap(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << kProg << " idmap: expected a subcommand: verify | reassign\n";
        return 2;
    }
    const std::string& sub = args[0];
    if (sub == "verify") {
        std::cout << "stub: idmap verify — check idmap.lock for collisions,"
                     " band violations and L015 drift (Tools SAD §2.4)\n";
        return 0;
    }
    if (sub == "reassign") {
        std::cout << "stub: idmap reassign — deterministically renumber entries"
                     " above released_watermark after a merge (Tools SAD §2.4)\n";
        return 0;
    }
    std::cerr << kProg << " idmap: unknown subcommand '" << sub
              << "' (expected verify | reassign)\n";
    return 2;
}

// ---- Command table ---------------------------------------------------------

const Command kCommands[] = {
    {"build",     "check then compile /content -> IF-4 SQL + IF-5 .pck (--full, --watch)", cmd_build},
    {"check",     "validate /content (or --file <p>): structural lints (L001-L011)", cmd_check},
    {"link",      "resolve refs + backlinks + allocate IF-9 ids (--report, --no-allocate-ids)", cmd_link},
    {"index",     "list the ID index: all ids by type + IF-9 numeric id (--json)",              cmd_index},
    {"pickable",  "typed ref picker: valid target ids for a ref type (pickable <type> --json)", cmd_pickable},
    {"refs",      "find-usages / backlinks for an id (refs <id> --json)",                        cmd_refs},
    {"emit-sql",  "emit IF-4 world DB SQL + world_manifest (--out <file>, --built-at)",         cmd_emit_sql},
    {"emit-pck",  "emit IF-5 client pack + pack.manifest.json (--out <dir>, --built-at)",       cmd_emit_pck},
    {"chunk-emit","emit a procedural N×N chunk fixture pack (IF-6) (--zone, --grid, --out)",     cmd_chunk_emit},
    {"fmt",       "canonically format /content YAML; --check for CI/pre-commit",       cmd_fmt},
    {"diff",      "classify pack changes additive/breaking: diff <old> <new> [--json]", cmd_diff},
    {"content-hash", "print the per-pack content_hash: content-hash [dir] [--json]",    cmd_content_hash},
    {"pack",      "build a signed .mcpack community pack + content hash",              cmd_pack},
    {"install",   "install a .mcpack into a realm: install <pack>",                    cmd_install},
    {"uninstall", "remove an installed pack from a realm: uninstall <pack>",           cmd_uninstall},
    {"migrate",   "upgrade a pack's YAML source to newer content schemas",            cmd_migrate},
    {"idmap",     "manage idmap.lock: idmap verify | idmap reassign",                  cmd_idmap},
};

void print_version() { std::cout << kProg << " " << MCC_VERSION << '\n'; }

void print_help() {
    std::cout
        << kProg << " " << MCC_VERSION
        << " — Meridian Content Compiler (TLS-01)\n\n"
           "One compiler, two outputs: world-DB SQL (IF-4) for the server and\n"
           "client .pck packs (IF-5). See docs/sad/tools-sad.md §2.\n\n"
           "USAGE:\n"
           "  " << kProg << " <command> [args]\n"
           "  " << kProg << " --version | --help\n\n"
           "COMMANDS:\n";
    // Longest command name is "content-hash" (12 chars); pad to a clean column.
    for (const auto& c : kCommands) {
        std::cout << "  " << c.name;
        for (std::size_t i = c.name.size(); i < 14; ++i) std::cout << ' ';
        std::cout << c.summary << '\n';
    }
    std::cout
        << "\nGLOBAL OPTIONS:\n"
           "  --version    print the mcc version and exit\n"
           "  --help, -h   print this help and exit\n\n"
           "CHECK/BUILD OPTIONS:\n"
           "  [dir]                content root to scan (default: ./content)\n"
           "  --diag-format=text   human-readable diagnostics (default)\n"
           "  --diag-format=json   structured diagnostics for Codex/Forge/CI\n\n"
           "CHECK SINGLE-FILE (validation-as-you-type, SAD §6.3):\n"
           "  --file <path>        validate ONE file, fast (sub-100ms editor loop);\n"
           "                       cross-file refs (L002/L010/L011) reported as\n"
           "                       deferred 'info' notes, never false errors. The\n"
           "                       [dir] positional is used only as root context\n"
           "                       for the diagnostic path.\n"
           "  --watch              (with --file) re-validate on change, streaming\n"
           "                       diagnostics until Ctrl-C (M1 half of TLS-06).\n\n"
           "FMT OPTIONS:\n"
           "  [path]               file or dir to format (default: ./content)\n"
           "  --check              report drift and exit non-zero; do not write\n\n"
           "LINK OPTIONS (SAD §2.3/§2.4):\n"
           "  [dir]                content root to link (default: ./content)\n"
           "  --report             print the allocated IF-9 idmap per namespace\n"
           "  --no-allocate-ids    read-only: never write idmap.lock; fail on L015\n"
           "                       drift (the CI contract). Default allocates +\n"
           "                       writes idmap.lock (editor-invoked builds).\n\n"
           "INDEX / PICKER / BACKLINK OPTIONS (SAD §2.3/§6, TLS-03):\n"
           "  index [dir] [--json]      the ID index — all content+asset ids grouped\n"
           "                            by type, each with its IF-9 numeric id + file\n"
           "                            (\"list all items\", id->numeric resolution).\n"
           "  pickable <type> [dir]     typed reference picker — the VALID target ids\n"
           "         [--json]           for a ref field of the given type (npc|item|\n"
           "                            quest|ability|loot|vendor|zone|art|mus|sfx|amb;\n"
           "                            \"itemRef\" or \"item\" both accepted).\n"
           "  refs <id> [dir] [--json]  find-usages / backlinks — who references <id>\n"
           "                            (the reverse graph), with each field path.\n"
           "                            All three run link read-only (no idmap write)\n"
           "                            and emit stable JSON with --json (deterministic,\n"
           "                            editor-consumable).\n\n"
           "EMIT-SQL OPTIONS (IF-4, SAD §2.6):\n"
           "  [dir]                content root to emit (default: ./content)\n"
           "  --out <file>         write the world DB SQL to <file> (default: stdout).\n"
           "                       With --out, diagnostics go to stdout; without it,\n"
           "                       the SQL is on stdout and diagnostics on stderr.\n"
           "  --built-at \"<ts>\"    world_manifest.built_at DATETIME (default a fixed\n"
           "                       epoch for reproducible output; pass a real build\n"
           "                       timestamp for a nightly build).\n"
           "                       Consumes the existing idmap.lock read-only — run\n"
           "                       'mcc link' / 'mcc build' first to allocate ids.\n\n"
           "EMIT-PCK OPTIONS (IF-5, SAD §2.7):\n"
           "  [dir]                content root to emit (default: ./content)\n"
           "  --out <dir>          write the pack under <dir>/meridian/<ns>/ (default:\n"
           "                       stdout gets pack.manifest.json, diagnostics stderr).\n"
           "  --built-at \"<ts>\"    pack.manifest.json built_at (default a fixed epoch\n"
           "                       for reproducible output).\n"
           "  --godot-version <v>  pinned engine version override (default: the pack's\n"
           "                       engine.godot). The manifest content_hash equals\n"
           "                       emit-sql's world_manifest hash (three-way tie).\n"
           "                       At M0 the pack payload is a documented directory\n"
           "                       manifest; the Godot-native .pck binary is a follow-up.\n\n"
           "CHUNK-EMIT OPTIONS (IF-6, SAD §3 — procedural fixture, #553):\n"
           "  --zone <id>          zone content id (default core:zone.zone01).\n"
           "  --grid <N>           emit an N×N chunk grid (default 3), centred on\n"
           "                       negative indices (Zone-01 spawns at x ≈ −300).\n"
           "  --origin-x/-z <m>    zone-local grid origin in metres (default -384).\n"
           "  --out <dir>          write the whole fixture under\n"
           "                       <dir>/meridian/<ns>/chunks/<zone>/ (manifest, IF-8\n"
           "                       asset table, IF-5 pack, per-chunk .chunk.bin/.tscn/\n"
           "                       .proxy.tscn). Without --out the IF-6 manifest is on\n"
           "                       stdout. Heightfields are deliberately NON-FLAT so\n"
           "                       downstream flat-vs-sloped bugs are catchable.\n"
           "  --built-at \"<ts>\"    pack.manifest.json built_at (fixed epoch default).\n"
           "  --godot-version <v>  engine pin recorded in the pack manifest.\n\n"
           "NOTE: discover/parse, the structural lints (L001-L011), the link stage\n"
           "(reference graph + backlinks + IF-9 idmap), the ID index / typed pickers /\n"
           "backlinks surface (index/pickable/refs), emit-sql (IF-4 world DB SQL +\n"
           "world_manifest), and emit-pck (IF-5 pack.manifest.json + M0 pack) are\n"
           "implemented; JSON Schema validation, semantic lints, bake, and the\n"
           "Godot-native .pck binary land in later M0 tasks (they report as stubs).\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        print_help();
        return 1;  // no command given — usage error
    }

    const std::string& first = args[0];

    if (first == "--version" || first == "-V") {
        print_version();
        return 0;
    }
    if (first == "--help" || first == "-h") {
        print_help();
        return 0;
    }

    for (const auto& c : kCommands) {
        if (first == c.name) {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            return c.run(rest);
        }
    }

    std::cerr << kProg << ": unknown command '" << first << "'\n"
              << "run '" << kProg << " --help' for the list of commands\n";
    return 2;
}
