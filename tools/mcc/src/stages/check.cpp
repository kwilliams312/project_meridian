// tools/mcc/src/stages/check.cpp — `mcc check` orchestration.

#include "stages/check.h"

#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/model.h"
#include "stages/parse.h"
#include "stages/validate.h"

namespace mcc::stages {

int check(const std::string& content_dir, DiagFormat format, std::ostream& out,
          std::ostream& err) {
    model::ContentModel model;
    if (!discover(content_dir, model)) {
        err << "mcc check: content directory not found: " << content_dir << '\n';
        return 2;
    }

    diag::Diagnostics diags;
    parse(model, diags);
    validate(model, diags);

    if (format == DiagFormat::Json) {
        diag::render_json(diags, out);
    } else {
        // Summary line mirrors validate_content.py's stats banner. Asset-ref and
        // full-corpus counts that depend on deferred layers are omitted here.
        std::string stats = "Validated " + std::to_string(model.file_count) +
                            " files: " + std::to_string(model.entity_count) +
                            " entities, " + std::to_string(model.asset_count) +
                            " asset sidecars, " + std::to_string(model.content_ref_count) +
                            " content refs.";
        diag::render_text(diags, stats, out);
    }

    return diags.ok() ? 0 : 1;
}

}  // namespace mcc::stages
