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

}  // namespace mcc::stages

#endif  // MCC_STAGES_VALIDATE_H
