// tools/mcc/src/stages/check.h — the `mcc check` orchestration.
//
// Runs discover -> parse -> validate over a content root and renders
// diagnostics as text or JSON. This is the real implementation behind
// `mcc check`, and the gate `mcc build` runs first (Tools SAD §2.1-2.2).

#ifndef MCC_STAGES_CHECK_H
#define MCC_STAGES_CHECK_H

#include <ostream>
#include <string>

namespace mcc::stages {

enum class DiagFormat { Text, Json };

// Run discover+parse+validate over `content_dir`, writing diagnostics to `out`.
// Returns 0 when there are no errors, 1 when any error is reported, and 2 when
// `content_dir` cannot be scanned (usage/environment error).
int check(const std::string& content_dir, DiagFormat format, std::ostream& out,
          std::ostream& err);

}  // namespace mcc::stages

#endif  // MCC_STAGES_CHECK_H
