// tools/mcc/src/stages/check.cpp — `mcc check` orchestration.

#include "stages/check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <system_error>
#include <thread>

#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/idmap.h"
#include "stages/link.h"
#include "stages/model.h"
#include "stages/parse.h"
#include "stages/validate.h"

namespace fs = std::filesystem;

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

int link_content(const std::string& content_dir, DiagFormat format, bool allocate_ids,
                 bool report, std::ostream& out, std::ostream& err) {
    model::ContentModel model;
    if (!discover(content_dir, model)) {
        err << "mcc link: content directory not found: " << content_dir << '\n';
        return 2;
    }

    diag::Diagnostics diags;
    parse(model, diags);
    validate(model, diags);
    // Only link when the model is structurally sound — a validate error means the
    // id universe / refs can't be trusted (SAD §2: validate gates the DAG). We
    // still run link so any *additional* link-only errors surface in one pass,
    // but idmap writes are suppressed unless validation passed.
    const bool validate_ok = diags.ok();
    // validate already emitted L011 for the corpus; link suppresses its own to
    // avoid a duplicate (emit_dangling=false), still counting danglers + keeping
    // them out of the graph. Link's unique output is the graph + backlinks + idmap.
    const LinkResult linked =
        link(model, content_dir, /*allocate=*/allocate_ids && validate_ok, diags,
             /*emit_dangling=*/false);

    if (format == DiagFormat::Json) {
        diag::render_json(diags, out);
    } else {
        std::string stats = "Linked " + std::to_string(model.file_count) +
                            " files: " + std::to_string(model.entity_count) +
                            " entities, " + std::to_string(model.asset_count) +
                            " asset sidecars, " + std::to_string(linked.edges.size()) +
                            " resolved refs, " + std::to_string(linked.dangling_count) +
                            " dangling.";
        diag::render_text(diags, stats, out);

        if (report) {
            for (const auto& [ns, m] : linked.idmaps) {
                out << "\nidmap [" << ns << "] band " << m.band << " — "
                    << m.map.size() << " ids (numeric = band*" << idmap::kBandStride
                    << "+index), " << m.retired.size() << " retired:\n";
                for (const auto& [id, idx] : m.map) {
                    out << "  " << id << " -> #" << idmap::numeric_id(m.band, idx)
                        << " (index " << idx << ")\n";
                }
            }
        }
    }

    return diags.ok() ? 0 : 1;
}

namespace {

// Resolve the diagnostic `rel_path` for a single file: root-relative when
// `content_root` is given and the file lives under it (matching `mcc check`),
// otherwise the path as provided on the command line.
std::string diag_rel_path(const fs::path& abs, const std::string& file_path,
                          const std::string& content_root) {
    if (content_root.empty()) return file_path;
    std::error_code ec;
    const fs::path root_abs = fs::absolute(fs::path(content_root), ec);
    if (ec) return file_path;
    const fs::path parent = root_abs.parent_path();
    const fs::path r = fs::relative(abs, parent, ec);
    if (ec || r.empty() || r.native().rfind("..", 0) == 0) return file_path;
    return r.generic_string();
}

// Validate one file in isolation and render the result to `out`. Returns 0 when
// clean, 1 on any error. `abs` must exist; the caller has already checked that.
int validate_and_render(const fs::path& abs, const std::string& rel_path,
                        DiagFormat format, std::ostream& out) {
    model::ParsedFile pf;
    pf.file.abs_path = abs.generic_string();
    pf.file.kind = classify(abs.filename().string(), pf.file.file_type);
    pf.file.rel_path = rel_path;

    // parse + single-file validate. Both degrade gracefully: a malformed file
    // yields a PARSE error, an unknown name yields L001, and every cross-file
    // rule becomes a deferred Info note (never a false error) — SAD §6.3.
    diag::Diagnostics diags;
    parse_file(pf, diags);
    validate_single_file(pf, diags);

    if (format == DiagFormat::Json) {
        diag::render_json(diags, "file", out);
    } else {
        const std::string stats =
            "Validated 1 file (single-file mode): " + pf.file.rel_path + ".";
        diag::render_text(diags, stats, out);
    }
    return diags.ok() ? 0 : 1;
}

}  // namespace

int check_file(const std::string& file_path, const std::string& content_root,
               DiagFormat format, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(file_path), ec);
    if (ec || !fs::exists(abs, ec) || !fs::is_regular_file(abs, ec)) {
        err << "mcc check: file not found: " << file_path << '\n';
        return 2;
    }
    return validate_and_render(abs, diag_rel_path(abs, file_path, content_root), format, out);
}

int watch_file(const std::string& file_path, const std::string& content_root,
               DiagFormat format, std::ostream& out, std::ostream& err,
               const std::function<bool()>& keep_running, unsigned poll_ms) {
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(file_path), ec);
    if (ec) {
        err << "mcc check: file not found: " << file_path << '\n';
        return 2;
    }
    const std::string rel = diag_rel_path(abs, file_path, content_root);

    // Poll the file's last-write time (dependency-free; the fs-event upgrade is
    // an M2 concern with the Forge --watch session, SAD §6.5). On each change we
    // re-validate and stream the result. A missing file is not fatal in watch
    // mode — the author may be mid-save; we report it once and keep watching.
    fs::file_time_type last{};
    bool have_last = false;
    bool reported_missing = false;

    do {
        std::error_code sec;
        const bool exists = fs::exists(abs, sec) && fs::is_regular_file(abs, sec);
        if (!exists) {
            if (!reported_missing) {
                err << "mcc check --watch: waiting for " << file_path << " ...\n";
                reported_missing = true;
            }
        } else {
            reported_missing = false;
            const fs::file_time_type mt = fs::last_write_time(abs, sec);
            if (sec) {
                // Transient stat failure (e.g. mid-rename): retry next tick.
            } else if (!have_last || mt != last) {
                last = mt;
                have_last = true;
                validate_and_render(abs, rel, format, out);
                out.flush();  // stream: an editor reads each render as it lands
            }
        }
        if (keep_running && !keep_running()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    } while (!keep_running || keep_running());

    return 0;
}

}  // namespace mcc::stages
