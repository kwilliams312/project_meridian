// tools/mcc/src/stages/check.cpp — `mcc check` orchestration.

#include "stages/check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <system_error>
#include <thread>

#include <fstream>

#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/emit_pck.h"
#include "stages/emit_sql.h"
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

int emit_sql_content(const std::string& content_dir, const std::string& out_file,
                     const std::string& mcc_version, const std::string& built_at,
                     DiagFormat format, std::ostream& out, std::ostream& err) {
    model::ContentModel model;
    if (!discover(content_dir, model)) {
        err << "mcc emit-sql: content directory not found: " << content_dir << '\n';
        return 2;
    }

    diag::Diagnostics diags;
    parse(model, diags);
    validate(model, diags);
    // emit-sql consumes the EXISTING idmap.lock (read-only: allocate=false). Ids
    // must already be allocated (`mcc build --allocate-ids` / `mcc link`) — a
    // drift surfaces as an L015 error and aborts the emit, exactly like CI.
    const LinkResult linked =
        link(model, content_dir, /*allocate=*/false, diags, /*emit_dangling=*/false);

    EmitSqlResult emitted;
    if (diags.ok()) {
        EmitSqlOptions opts;
        opts.mcc_version = mcc_version;
        opts.built_at = built_at;
        emitted = emit_sql(model, linked, opts, diags);
    }

    // Diagnostics render to stderr-ish `out` UNLESS the SQL itself goes to stdout
    // (then diagnostics go to `err` so the emitted SQL is clean on stdout).
    std::ostream& diag_out = out_file.empty() ? err : out;
    if (format == DiagFormat::Json) {
        diag::render_json(diags, diag_out);
    } else {
        std::string stats = "Emitted world DB SQL: " +
                            std::to_string(emitted.manifest_rows) + " manifest row(s), " +
                            std::to_string(emitted.content_rows) + " content rows.";
        diag::render_text(diags, stats, diag_out);
    }

    if (!diags.ok()) {
        err << "mcc emit-sql: check/link/emit failed — no SQL written\n";
        return 1;
    }

    if (out_file.empty()) {
        out << emitted.sql;
    } else {
        // Create the parent directory if it does not exist (e.g. `mcc build`
        // writes build/world.sql into a fresh tree).
        std::error_code ec;
        const fs::path parent = fs::path(out_file).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        std::ofstream f(out_file, std::ios::binary | std::ios::trunc);
        if (!f) {
            err << "mcc emit-sql: could not open output file: " << out_file << '\n';
            return 2;
        }
        f << emitted.sql;
    }
    return 0;
}

int emit_pck_content(const std::string& content_dir, const std::string& out_dir,
                     const std::string& mcc_version, const std::string& built_at,
                     const std::string& godot_version, DiagFormat format,
                     std::ostream& out, std::ostream& err) {
    model::ContentModel model;
    if (!discover(content_dir, model)) {
        err << "mcc emit-pck: content directory not found: " << content_dir << '\n';
        return 2;
    }

    diag::Diagnostics diags;
    parse(model, diags);
    validate(model, diags);
    // emit-pck consumes the EXISTING idmap.lock (read-only: allocate=false), same
    // as emit-sql — the entry numeric ids must match the SQL keys exactly, so both
    // stages read the identical allocated idmap. A drift is an L015 error.
    const LinkResult linked =
        link(model, content_dir, /*allocate=*/false, diags, /*emit_dangling=*/false);

    EmitPckResult emitted;
    if (diags.ok()) {
        EmitPckOptions opts;
        opts.mcc_version = mcc_version;
        opts.built_at = built_at;
        opts.godot_version = godot_version;
        emitted = emit_pck(model, linked, opts, diags);
    }

    // Diagnostics render to `out` when writing files (out_dir set), else to `err`
    // so the manifest is clean on stdout.
    std::ostream& diag_out = out_dir.empty() ? err : out;
    if (format == DiagFormat::Json) {
        diag::render_json(diags, diag_out);
    } else {
        std::string stats = "Emitted client pack '" + emitted.pack_namespace +
                            "': pack.manifest.json + " +
                            std::to_string(emitted.entries.size()) + " entries (content_hash " +
                            (emitted.content_hash.size() >= 12 ? emitted.content_hash.substr(0, 12)
                                                               : emitted.content_hash) +
                            "...).";
        diag::render_text(diags, stats, diag_out);
    }

    if (!diags.ok()) {
        err << "mcc emit-pck: check/link/emit failed — no pack written\n";
        return 1;
    }

    if (out_dir.empty()) {
        // No output dir: the manifest (the IF-5 contract) goes to stdout.
        out << emitted.manifest_json;
    } else {
        // Write pack.manifest.json + the M0 directory-manifest pack into
        // <out_dir>/meridian/<ns>/ (mirroring the res:// layout root).
        std::error_code ec;
        const fs::path pack_root =
            fs::path(out_dir) / "meridian" / emitted.pack_namespace;
        fs::create_directories(pack_root, ec);
        if (ec) {
            err << "mcc emit-pck: could not create output dir: " << pack_root.string()
                << '\n';
            return 2;
        }
        {
            std::ofstream f(pack_root / "pack.manifest.json",
                            std::ios::binary | std::ios::trunc);
            if (!f) {
                err << "mcc emit-pck: could not write pack.manifest.json\n";
                return 2;
            }
            f << emitted.manifest_json;
        }
        {
            std::ofstream f(pack_root / "pack.contents.jsonl",
                            std::ios::binary | std::ios::trunc);
            if (!f) {
                err << "mcc emit-pck: could not write pack.contents.jsonl\n";
                return 2;
            }
            f << emitted.contents_jsonl;
        }
    }
    return 0;
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
