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
