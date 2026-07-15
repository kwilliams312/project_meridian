// tools/mcc/src/stages/check.h — the `mcc check` orchestration.
//
// Runs discover -> parse -> validate over a content root and renders
// diagnostics as text or JSON. This is the real implementation behind
// `mcc check`, and the gate `mcc build` runs first (Tools SAD §2.1-2.2).

#ifndef MCC_STAGES_CHECK_H
#define MCC_STAGES_CHECK_H

#include <functional>
#include <ostream>
#include <string>

namespace mcc::stages {

enum class DiagFormat { Text, Json };

// Run discover+parse+validate over `content_dir`, writing diagnostics to `out`.
// Returns 0 when there are no errors, 1 when any error is reported, and 2 when
// `content_dir` cannot be scanned (usage/environment error).
int check(const std::string& content_dir, DiagFormat format, std::ostream& out,
          std::ostream& err);

// Run discover+parse+validate+link over `content_dir` (Tools SAD §2.1-2.4):
// the full front half of the compiler DAG. On top of `check`, this resolves the
// reference graph, builds backlinks, and allocates IF-9 numeric ids per pack via
// idmap.lock. `allocate_ids` controls the idmap.lock write policy (SAD §2.3):
// true (editor-invoked builds) writes updated locks; false (CI) runs read-only
// and fails on L015 drift. When `report` is true, a human-readable idmap summary
// (allocated ids per namespace) is printed after the diagnostics. Returns 0 on
// no errors, 1 on any error, 2 when `content_dir` cannot be scanned.
int link_content(const std::string& content_dir, DiagFormat format, bool allocate_ids,
                 bool report, std::ostream& out, std::ostream& err);

// Run discover+parse+validate+link then the emit-sql stage (Tools SAD §2.6, IF-4).
// Emits the world DB DML (content-table inserts + the one `world_manifest` row
// worldd reads at boot) to `out_file` when non-empty, else to `out`. The front
// half runs read-only (never writes idmap.lock — emit consumes the existing lock;
// a drift is an L015 error and aborts). `mcc_version`/`built_at` are stamped into
// world_manifest (built_at is a fixed/parameterized value, never the wall clock,
// so the emit is reproducible). Diagnostics render to `out` unless emitting there.
// Returns 0 on success, 1 on any error, 2 when `content_dir` cannot be scanned or
// `out_file` cannot be written.
int emit_sql_content(const std::string& content_dir, const std::string& out_file,
                     const std::string& mcc_version, const std::string& built_at,
                     DiagFormat format, std::ostream& out, std::ostream& err);

// Run discover+parse+validate+link then the emit-pck stage (Tools SAD §2.7, IF-5).
// Assembles the client content pack: pack.manifest.json (the IF-5 metadata the
// client mounts + verifies) plus an M0 directory-manifest pack payload. When
// `out_dir` is non-empty, both files are written under
// <out_dir>/meridian/<namespace>/ (mirroring the res:// layout root); when empty,
// pack.manifest.json goes to `out` and diagnostics to `err`. Like emit-sql, the
// front half runs read-only (never writes idmap.lock — the entry numeric ids MUST
// match the SQL keys, so both stages read the identical allocated idmap; a drift
// is an L015 error). `content_hash` in the manifest is byte-identical to
// emit-sql's world_manifest hash (the three-way tie, SAD §2.6). `mcc_version`/
// `built_at` are stamped in (built_at parameterized, never the wall clock);
// `godot_version` overrides the pack's engine.godot pin when non-empty. Returns 0
// on success, 1 on any error, 2 when `content_dir` cannot be scanned or `out_dir`
// cannot be written.
// `select_namespace` picks which pack to emit when the tree holds more than one
// (empty = first sorted by namespace; an unknown namespace is an error).
int emit_pck_content(const std::string& content_dir, const std::string& out_dir,
                     const std::string& mcc_version, const std::string& built_at,
                     const std::string& godot_version, DiagFormat format,
                     std::ostream& out, std::ostream& err,
                     const std::string& select_namespace = "");

// Fast single-file / incremental validation path (Tools SAD §6.3, TLS-06): run
// classify + parse + the single-file lints over exactly ONE content file, with
// cross-file rules (L002/L010/L011) degraded to deferred `Info` notes rather
// than false errors. This is the validation-as-you-type contract an editor / LSP
// drives per keystroke (sub-100ms; no directory walk, no corpus load).
//
// `content_root` is optional context: when non-empty and `file_path` lives under
// it, the diagnostic `file` field is made root-relative (matching `mcc check`'s
// paths) — otherwise the file's own path is used verbatim. Returns 0 when there
// are no *errors* (deferred/info notes never fail), 1 on any error, 2 when
// `file_path` cannot be read (usage/environment error).
int check_file(const std::string& file_path, const std::string& content_root,
               DiagFormat format, std::ostream& out, std::ostream& err);

// Watch a single file and re-run `check_file`'s validation on every change,
// streaming each result to `out` (the M1 half of the TLS-06 loop; the fs-event
// + Forge-attached session upgrade is M2, SAD §6.5). Dependency-free polling of
// the file's mtime every `poll_ms`. `keep_running`, when set, is polled each
// tick to allow a bounded run (the CLI passes an empty function → runs until the
// process is signalled; tests pass a predicate to make the loop finite). Returns
// 0. Errors in any single validation are streamed, not fatal — the author is
// expected to fix and re-save.
int watch_file(const std::string& file_path, const std::string& content_root,
               DiagFormat format, std::ostream& out, std::ostream& err,
               const std::function<bool()>& keep_running = {}, unsigned poll_ms = 200);

}  // namespace mcc::stages

#endif  // MCC_STAGES_CHECK_H
