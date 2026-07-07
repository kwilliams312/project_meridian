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
#include "stages/format.h"
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

    std::cout << "stub: build — bake + emit (Tools SAD §2),"
                 " producing IF-4 SQL + IF-5 .pck\n";
    std::cout << "  mode: " << (full ? "full rebuild" : "incremental")
              << (watch ? ", watching for changes" : "") << '\n';
    // discover/parse/validate/link are now real; bake/emit remain stubs until
    // later M0 tasks (#120 emit-sql, #121 emit-pck).
    emit_stage(mcc::stages::Stage::Bake);
    emit_stage(mcc::stages::Stage::EmitSql);
    emit_stage(mcc::stages::Stage::EmitPck);
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

int cmd_diff(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cerr << kProg << " diff: expected two build arguments: "
                  << "diff <buildA> <buildB>\n";
        return 2;
    }
    std::cout << "stub: diff — compare two builds (" << args[0] << " vs "
              << args[1] << ") at the manifest/artifact level (Tools PRD §2.2)\n";
    return 0;
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
    {"fmt",       "canonically format /content YAML; --check for CI/pre-commit",       cmd_fmt},
    {"diff",      "compare two builds: diff <buildA> <buildB>",                        cmd_diff},
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
    // Longest command name is "uninstall" (9 chars); pad to a clean column.
    for (const auto& c : kCommands) {
        std::cout << "  " << c.name;
        for (std::size_t i = c.name.size(); i < 11; ++i) std::cout << ' ';
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
           "NOTE: discover/parse, the structural lints (L001-L011), and the link\n"
           "stage (reference graph + backlinks + IF-9 idmap) are implemented; JSON\n"
           "Schema validation, semantic lints, and bake/emit land in later M0 tasks\n"
           "(they report as stubs).\n";
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
