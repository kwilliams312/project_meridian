// tools/mcc/src/stages/parse.h — parse stage (Tools SAD §2.1).
//
// Loads each discovered file with yaml-cpp and reads the envelope (`schema:`,
// `id:`; `namespace:` for pack manifests). Malformed YAML, an empty document,
// or a non-mapping root are reported as PARSE diagnostics and the file is left
// `parsed = false` — never a crash (matching the reference validator's
// robustness). Envelope-first so version dispatch can precede typed decoding
// (SAD §2.1); typed per-schema decoding lands with schema-generated structs.

#ifndef MCC_STAGES_PARSE_H
#define MCC_STAGES_PARSE_H

#include "stages/diagnostics.h"
#include "stages/model.h"

namespace mcc::stages {

// Parse ONE already-classified file in place: load its YAML, and on success set
// `pf.parsed`, `pf.root`, and the envelope fields (`schema`/`id`, plus
// `namespace_` for pack manifests). Malformed / empty / non-mapping documents
// get a PARSE diagnostic (with a 1-based line when yaml-cpp reports one) and
// leave `pf.parsed == false`. FileKind::Unknown files are left untouched (the
// validate stage reports them as L001). Never throws. Shared by the full
// `parse()` sweep and the single-file `check --file` path (SAD §6.3).
void parse_file(model::ParsedFile& pf, diag::Diagnostics& diags);

// Parse every file in `model.files` in place. Files whose name did not classify
// (FileKind::Unknown) are skipped here — the validate stage reports them as
// L001. Emits PARSE diagnostics for malformed / empty / non-mapping documents.
void parse(model::ContentModel& model, diag::Diagnostics& diags);

}  // namespace mcc::stages

#endif  // MCC_STAGES_PARSE_H
