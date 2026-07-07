// tools/mcc/src/stages/validate.h — validate stage (Tools SAD §2.2).
//
// Structural lint layer, implemented to match tools/validate_content.py verdict
// for verdict on the rules in scope this round:
//
//   L001  filename / declared-envelope-type mismatch
//   L002  id namespace != owning pack namespace
//   L003  id type segment != the file's schema type
//   L010  duplicate ids across the content tree
//   L011  content reference (npc./item./...) resolves to a defined id
//
// Deferred (reported once as an informational note, not run this round):
//   - Full JSON Schema 2020-12 validation (the SCHEMA layer)
//   - L004 intRange min<=max, L020 asset-sidecar existence,
//     L034/L035/L052/L062 semantic lints
// These arrive with schema-generated decoding / the link stage in later tasks.

#ifndef MCC_STAGES_VALIDATE_H
#define MCC_STAGES_VALIDATE_H

#include "stages/diagnostics.h"
#include "stages/model.h"

namespace mcc::stages {

// Run the structural lint engine over the parsed model, appending diagnostics.
// Updates `model` stats (entity/asset/content_ref counts) used by the summary.
void validate(model::ContentModel& model, diag::Diagnostics& diags);

// Single-file / validation-as-you-type path (Tools SAD §6.3): validate exactly
// ONE parsed file in isolation, without the cross-file corpus. Runs every lint
// that is computable from the file alone — L001 (filename/envelope), L003 (id
// type segment) — using the SAME rule logic as `validate()` above (no
// duplicated verdicts). Cross-file rules that need the full DAG are DEGRADED
// GRACEFULLY, not skipped silently: each content reference is emitted as an
// `Info` note (rule L011, "deferred — needs full check"), and L002/L010 are
// noted once as deferred, so an editor shows what is checkable now and clearly
// marks what a full `mcc check` must confirm. Never turns an
// unresolved-in-isolation reference into a false error. Updates `pf` in place is
// not required; `pf` must already be parsed (`pf.parsed == true`) or carry the
// PARSE/L001 diagnostic from the parse stage.
void validate_single_file(const model::ParsedFile& pf, diag::Diagnostics& diags);

}  // namespace mcc::stages

#endif  // MCC_STAGES_VALIDATE_H
