// tools/mcc/src/stages/format.h — the `fmt` stage: canonical YAML emitter.
//
// `mcc fmt` re-serializes a hand-authored /content YAML file into ONE canonical
// form (Tools PRD §2.1 "canonical key order enforced by a formatter"; Tools SAD
// §5.3/§6.2 "the same mcc fmt emitter"). The point is diff-friendliness: same
// content ⇒ byte-identical bytes, so Git diffs are semantic, not cosmetic, and
// CI / pre-commit can enforce one form (`mcc fmt --check`, PRD §7.2).
//
// Two hard guarantees, both property-tested (see tools/mcc/tests):
//   1. IDEMPOTENT:  format(format(x)) == format(x).
//   2. SEMANTIC-PRESERVING: parse(format(x)) is node-identical to parse(x) —
//      keys, scalar values, and resolved scalar TYPES are unchanged. Quoting is
//      chosen so a string that looks like a number/bool/null (e.g. "4.6", "01",
//      "on") stays a string, and a bare number/bool/null keeps its type.
//
// Comments are preserved: leading full-line comments and trailing end-of-line
// comments are captured from the source and re-attached to the node they
// annotate (by source line), so hand-annotated files survive a reformat.
//
// Canonical-form rules are documented in tools/mcc/FORMAT.md and enforced here.

#ifndef MCC_STAGES_FORMAT_H
#define MCC_STAGES_FORMAT_H

#include <ostream>
#include <string>

namespace mcc::stages {

// Outcome of formatting one source buffer.
struct FormatResult {
    bool ok = false;            // false when `input` was not parseable YAML
    std::string output;         // canonical bytes (valid only when ok)
    std::string error;          // human-readable reason when !ok
};

// Format a single YAML document held in `input` (the raw file bytes) into its
// canonical form. `input` must be one YAML mapping document (the content
// envelope shape); a non-mapping / empty / malformed document yields ok=false.
FormatResult format_yaml(const std::string& input);

// Read `path`, format it, and behave per mode:
//   check_only=true : do not write; return 0 if already canonical, 1 if not
//                     (a "would reformat" note is printed to `out`).
//   check_only=false: rewrite `path` in place if it differs; print nothing on a
//                     no-op, a "formatted" note when it changed.
// Returns 0 on success/clean, 1 when --check found drift, 2 on an I/O or parse
// error (message on `err`). This is the per-file worker behind `mcc fmt`.
int format_file(const std::string& path, bool check_only, std::ostream& out,
                std::ostream& err);

// Run `mcc fmt` over a path that may be a single file or a directory tree.
// When `path` is a directory, every *.yaml under it is formatted (sorted, for
// deterministic output order). `check_only` maps to `--check`. Returns 0 when
// everything is clean/formatted, 1 when --check found any drift, 2 on error.
int fmt(const std::string& path, bool check_only, std::ostream& out, std::ostream& err);

}  // namespace mcc::stages

#endif  // MCC_STAGES_FORMAT_H
