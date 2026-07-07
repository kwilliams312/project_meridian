// tools/mcc/src/stages/diagnostics.h — structured diagnostics for the pipeline.
//
// Every stage produces `Diagnostic`s rather than printing directly, so the same
// data can be rendered as human text or as JSON (`--diag-format=json`) for
// Codex / Forge / CI consumption (Tools SAD §2.2). The shape mirrors the SAD's
// diagnostic tuple: rule id, severity, entity/field context, file location, and
// a fix hint carried in `message`.

#ifndef MCC_STAGES_DIAGNOSTICS_H
#define MCC_STAGES_DIAGNOSTICS_H

#include <ostream>
#include <string>
#include <vector>

namespace mcc::diag {

// `Info` is a non-failing note (severity below `Warning`). It is used by the
// single-file / validation-as-you-type path (`mcc check --file`) to surface a
// check that is *deferred* — i.e. only computable with the full cross-file DAG
// (a content reference's L011 resolution, duplicate-id L010, pack-ownership
// L002) — without turning an unresolved-in-isolation ref into a false error.
enum class Severity { Error, Warning, Info };

// One diagnostic. `rule` is the permanent lint id (e.g. "L001", "PARSE").
// `file` is a repo-root-relative path (forward slashes, per SAD §9.4). `where`
// is an optional field/location note (e.g. "$.objectives[0]") — a schema
// json-path an editor maps back to the offending form control (SAD §6.3).
// `line`/`col` are 1-based source coordinates when known (0 = unknown), so an
// editor can place a squiggle without re-parsing.
struct Diagnostic {
    std::string rule;
    Severity severity = Severity::Error;
    std::string file;
    std::string where;
    std::string message;
    int line = 0;   // 1-based; 0 = unknown
    int col = 0;    // 1-based; 0 = unknown
};

// Accumulates diagnostics across stages and answers "did anything fail?".
class Diagnostics {
public:
    void error(std::string rule, std::string file, std::string where, std::string message,
               int line = 0, int col = 0) {
        items_.push_back({std::move(rule), Severity::Error, std::move(file),
                          std::move(where), std::move(message), line, col});
    }
    void warning(std::string rule, std::string file, std::string where, std::string message,
                 int line = 0, int col = 0) {
        items_.push_back({std::move(rule), Severity::Warning, std::move(file),
                          std::move(where), std::move(message), line, col});
    }
    // A deferred / informational note (does not affect `ok()` / exit code).
    void info(std::string rule, std::string file, std::string where, std::string message,
              int line = 0, int col = 0) {
        items_.push_back({std::move(rule), Severity::Info, std::move(file),
                          std::move(where), std::move(message), line, col});
    }

    const std::vector<Diagnostic>& items() const { return items_; }

    std::size_t count_of(Severity sev) const {
        std::size_t n = 0;
        for (const auto& d : items_) {
            if (d.severity == sev) ++n;
        }
        return n;
    }
    std::size_t error_count() const { return count_of(Severity::Error); }
    std::size_t warning_count() const { return count_of(Severity::Warning); }
    std::size_t info_count() const { return count_of(Severity::Info); }

    // Only errors fail the run. Warnings and info notes are advisory.
    bool ok() const { return error_count() == 0; }

private:
    std::vector<Diagnostic> items_;
};

// Format one line matching the reference validator's text form, e.g.
//   "L011 core/quests/foo.quest.yaml at $.giver: unresolved reference 'npc.x'"
// The optional " at <where>" clause is emitted only when `where` is non-empty.
std::string format_text(const Diagnostic& d);

// Render all diagnostics + a summary as human-readable text to `out`.
// `stats_line` is the "Validated N files: ..." summary printed first.
void render_text(const Diagnostics& diags, const std::string& stats_line, std::ostream& out);

// Render all diagnostics as a single JSON object to `out` (--diag-format=json).
// The overload tags the envelope with a `mode` ("check" full-corpus vs "file"
// single-file); the one-arg form defaults to "check" for the full `mcc check`.
void render_json(const Diagnostics& diags, std::ostream& out);
void render_json(const Diagnostics& diags, const std::string& mode, std::ostream& out);

}  // namespace mcc::diag

#endif  // MCC_STAGES_DIAGNOSTICS_H
