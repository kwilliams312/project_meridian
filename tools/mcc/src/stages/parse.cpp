// tools/mcc/src/stages/parse.cpp — parse stage implementation.

#include "stages/parse.h"

#include <string>

#include <yaml-cpp/yaml.h>

namespace mcc::stages {

namespace {

// Read a scalar string field from a mapping node, or "" if absent/non-scalar.
std::string scalar_field(const YAML::Node& map, const char* key) {
    if (!map.IsMap()) return "";
    const YAML::Node v = map[key];
    if (v && v.IsScalar()) return v.as<std::string>();
    return "";
}

}  // namespace

void parse_file(model::ParsedFile& pf, diag::Diagnostics& diags) {
    // Unknown-named files are handled as L001 by the validate stage; the
    // reference validator never parses them, so neither do we.
    if (pf.file.kind == model::FileKind::Unknown) return;

    YAML::Node root;
    try {
        root = YAML::LoadFile(pf.file.abs_path);
    } catch (const YAML::Exception& ex) {
        // yaml-cpp carries a 1-based line/col in ex.mark when available.
        std::string where;
        int line = 0, col = 0;
        if (ex.mark.line >= 0) {
            line = ex.mark.line + 1;
            col = ex.mark.column >= 0 ? ex.mark.column + 1 : 0;
            where = "line " + std::to_string(line);
        }
        diags.error("PARSE", pf.file.rel_path, where, "invalid YAML", line, col);
        return;
    }

    // Empty document (null root) or a non-mapping root is a PARSE error,
    // matching validate_content.py's `not isinstance(doc, dict)` check.
    if (!root || root.IsNull() || !root.IsMap()) {
        diags.error("PARSE", pf.file.rel_path, "", "document is not a YAML mapping");
        return;
    }

    pf.parsed = true;
    pf.root = root;
    pf.schema = scalar_field(root, "schema");
    pf.id = scalar_field(root, "id");
    if (pf.file.kind == model::FileKind::Pack) {
        pf.namespace_ = scalar_field(root, "namespace");
    }
}

void parse(model::ContentModel& model, diag::Diagnostics& diags) {
    for (auto& pf : model.files) {
        parse_file(pf, diags);
    }
}

}  // namespace mcc::stages
