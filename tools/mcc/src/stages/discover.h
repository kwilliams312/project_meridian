// tools/mcc/src/stages/discover.h — discover stage (Tools SAD §2.1).
//
// Walks a /content root, finds every *.yaml file, and classifies each by name
// (pack.yaml / <name>.<type>.yaml / <name>.asset.yaml). File iteration is
// sorted (SAD §9.4: never trust readdir order) and paths are normalized to
// forward slashes for stable, cross-platform diagnostics.

#ifndef MCC_STAGES_DISCOVER_H
#define MCC_STAGES_DISCOVER_H

#include <string>

#include "stages/model.h"

namespace mcc::stages {

// The eight content types (schema README) plus the "asset" sidecar token.
// Exposed for reuse by the validate stage (L003 type-segment check).
bool is_content_type(const std::string& t);

// Classify a bare filename (no directory) into its FileKind + file_type token.
// Matches validate_content.py::file_type: "pack.yaml" -> Pack; a three-part
// "<name>.<type>.yaml" whose middle segment is a content type or "asset" ->
// Content/Asset; anything else -> Unknown.
model::FileKind classify(const std::string& filename, std::string& file_type_out);

// Walk `content_dir` for *.yaml files (recursively, sorted by relative path),
// populating `model.files` with DiscoveredFile entries and `model.content_dir`
// / `model.root_dir`. Returns false if `content_dir` does not exist.
bool discover(const std::string& content_dir, model::ContentModel& model);

}  // namespace mcc::stages

#endif  // MCC_STAGES_DISCOVER_H
