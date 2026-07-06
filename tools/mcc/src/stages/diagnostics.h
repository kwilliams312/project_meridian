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

enum class Severity { Error, Warning };

// One diagnostic. `rule` is the permanent lint id (e.g. "L001", "PARSE").
// `file` is a repo-root-relative path (forward slashes, per SAD §9.4). `where`
// is an optional field/location note (e.g. "$.objectives[0]") or a YAML line
// hint; empty when not applicable.
struct Diagnostic {
    std::string rule;
    Severity severity = Severity::Error;
    std::string file;
    std::string where;
    std::string message;
};

// Accumulates diagnostics across stages and answers "did anything fail?".
class Diagnostics {
public:
    void error(std::string rule, std::string file, std::string where, std::string message) {
        items_.push_back({std::move(rule), Severity::Error, std::move(file),
                          std::move(where), std::move(message)});
    }
    void warning(std::string rule, std::string file, std::string where, std::string message) {
        items_.push_back({std::move(rule), Severity::Warning, std::move(file),
                          std::move(where), std::move(message)});
    }

    const std::vector<Diagnostic>& items() const { return items_; }

    std::size_t error_count() const {
        std::size_t n = 0;
        for (const auto& d : items_) {
            if (d.severity == Severity::Error) ++n;
        }
        return n;
    }
    std::size_t warning_count() const { return items_.size() - error_count(); }

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
void render_json(const Diagnostics& diags, std::ostream& out);

}  // namespace mcc::diag

#endif  // MCC_STAGES_DIAGNOSTICS_H
