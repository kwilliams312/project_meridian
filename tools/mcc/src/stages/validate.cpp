// tools/mcc/src/stages/validate.cpp — structural lint engine.
//
// Parity target: tools/validate_content.py. The control flow, rule ids, and
// message text below deliberately mirror that reference so `mcc check` and the
// Python validator return the same verdicts on the rules in scope (L001, L002,
// L003, L010, L011). Divergence from the reference is limited to the layers
// explicitly deferred this round (JSON Schema validation, L004/L020/semantic
// lints) — see validate.h.

#include "stages/validate.h"

#include <algorithm>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "stages/discover.h"

namespace mcc::stages {

namespace {

// Content reference grammar (validate_content.py REF_RE): an optional
// "<namespace>:" prefix followed by "<type>.<segment>(.<segment>)*" where the
// type is one of the eight content types. Anchored; std::regex has no ^$ default.
const std::regex kRefRe(
    R"(^(?:([a-z][a-z0-9_]{1,31}):)?((npc|item|quest|ability|loot|vendor|spawn|zone)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)$)");

// A content reference discovered in a file, ready for L011 resolution.
struct Ref {
    std::string rel_path;   // owning file, for the diagnostic
    std::string where;      // json-path location (e.g. "$.objectives[0].target")
    std::string resolved;   // namespace-qualified id ("core:npc.kobold_miner")
};

// Resolve a reference to a fully-qualified id: refs without a "<ns>:" prefix
// default to the owning file's namespace (schema README §References).
std::string resolve(const std::string& ref, const std::string& namespace_) {
    return ref.find(':') != std::string::npos ? ref : namespace_ + ":" + ref;
}

// Walk every scalar string in a YAML tree, yielding (json-path, value), skipping
// the top-level `id` (the definition, not a reference) — matches walk_strings.
void walk_strings(const YAML::Node& node, const std::string& path, bool top_level,
                  std::vector<std::pair<std::string, std::string>>& out) {
    if (node.IsMap()) {
        for (const auto& kv : node) {
            const std::string key = kv.first.as<std::string>();
            if (top_level && key == "id") continue;
            walk_strings(kv.second, path + "." + key, false, out);
        }
    } else if (node.IsSequence()) {
        for (std::size_t i = 0; i < node.size(); ++i) {
            walk_strings(node[i], path + "[" + std::to_string(i) + "]", false, out);
        }
    } else if (node.IsScalar()) {
        out.emplace_back(path, node.Scalar());
    }
}

// Asset type prefixes — used only to classify an asset id's type segment for
// L003 (the asset sidecar case), mirroring validate_content.py ASSET_PREFIXES.
bool is_asset_prefix(const std::string& t) {
    return t == "art" || t == "mus" || t == "sfx" || t == "amb";
}

// True if `descendant` is `ancestor` or lies beneath it (path-prefix test on
// normalized generic paths). Both are absolute forward-slash directory strings.
bool is_under(const std::string& descendant, const std::string& ancestor_dir) {
    if (descendant == ancestor_dir) return true;
    if (descendant.size() <= ancestor_dir.size()) return false;
    if (descendant.compare(0, ancestor_dir.size(), ancestor_dir) != 0) return false;
    return descendant[ancestor_dir.size()] == '/';
}

}  // namespace

void validate(model::ContentModel& model, diag::Diagnostics& diags) {
    // Pack directories -> declared namespace (Pass 1 populates in file order,
    // matching the Python dict's insertion order for the L002 "first ancestor".)
    std::vector<std::pair<std::string, std::string>> pack_namespaces;  // (dir, ns)

    std::unordered_map<std::string, std::string> ids;      // id -> rel_path (first def)
    std::unordered_set<std::string> content_ids;            // L011-resolvable ids
    std::size_t asset_count = 0;
    std::vector<Ref> refs;

    // Track (id -> owning file dir) for L002, in first-definition order.
    std::vector<std::pair<std::string, std::string>> id_dirs;  // (id, file_dir)

    // ---- Pass 1: L001 envelope, L003 type segment, L010 dupes, collect refs.
    for (const auto& pf : model.files) {
        const auto& f = pf.file;

        // L001a — filename did not classify.
        if (f.kind == model::FileKind::Unknown) {
            diags.error("L001", f.rel_path, "",
                        "filename must be '<name>.<type>.yaml' or 'pack.yaml'");
            continue;
        }
        // PARSE files were already reported and carry no envelope.
        if (!pf.parsed) continue;

        // L001b — declared envelope type must match the filename's type token.
        const std::string expected = "meridian/" + f.file_type + "@1";
        if (pf.schema != expected) {
            diags.error("L001", f.rel_path, "",
                        "declares '" + pf.schema + "', expected '" + expected + "'");
            continue;
        }

        // NOTE: JSON Schema 2020-12 validation (the SCHEMA layer) runs here in
        // the reference validator. Deferred this round (see validate.h) — the
        // structural lints below run directly on the parsed envelope + tree.

        // The owning directory of this file (for L002 pack ownership).
        const std::string file_dir =
            f.abs_path.substr(0, f.abs_path.find_last_of('/'));

        if (f.kind == model::FileKind::Pack) {
            pack_namespaces.emplace_back(file_dir, pf.namespace_);
            continue;
        }

        // From here every file is a content or asset definition with an `id`.
        const std::string& doc_id = pf.id;
        const std::size_t colon = doc_id.find(':');
        if (colon == std::string::npos) {
            // Malformed id (no namespace). JSON Schema would have rejected this;
            // with schema validation deferred, guard rather than crash. Reported
            // as L003 (closest structural rule) so the file is still flagged.
            diags.error("L003", f.rel_path, "",
                        "id '" + doc_id + "' is not of the form '<namespace>:<type>.<name>'");
            continue;
        }
        const std::string local = doc_id.substr(colon + 1);
        const std::string type_segment = local.substr(0, local.find('.'));

        // L003 — id type segment must match the file's schema type.
        if (f.kind == model::FileKind::Asset) {
            if (!is_asset_prefix(type_segment)) {
                diags.error("L003", f.rel_path, "",
                            "asset id must use an art/mus/sfx/amb prefix, got '" +
                                type_segment + "'");
                continue;
            }
        } else if (type_segment != f.file_type) {
            diags.error("L003", f.rel_path, "",
                        "id type segment '" + type_segment +
                            "' does not match schema type '" + f.file_type + "'");
            continue;
        }

        // L010 — duplicate id (first definition wins for resolution).
        auto existing = ids.find(doc_id);
        if (existing != ids.end()) {
            diags.error("L010", f.rel_path, "",
                        "duplicate id '" + doc_id + "' (also in " + existing->second + ")");
            continue;
        }
        ids.emplace(doc_id, f.rel_path);
        id_dirs.emplace_back(doc_id, file_dir);

        if (f.kind == model::FileKind::Asset) {
            ++asset_count;
            continue;  // sidecar-internal refs are metadata, not L011 refs
        }

        content_ids.insert(doc_id);

        // Collect content references from every scalar (skips top-level id).
        const std::string namespace_ = doc_id.substr(0, colon);
        std::vector<std::pair<std::string, std::string>> strings;
        walk_strings(pf.root, "$", /*top_level=*/true, strings);
        for (const auto& [loc, value] : strings) {
            std::smatch m;
            if (std::regex_match(value, m, kRefRe)) {
                const std::string ns = m[1].matched ? m[1].str() : namespace_;
                refs.push_back({f.rel_path, loc, resolve(m[2].str(), ns)});
            }
            // Asset refs (ASSET_RE / L020) are deferred this round.
        }
    }

    // ---- Pass 2: L002 — nearest ancestor pack.yaml owns the file's namespace.
    for (const auto& [doc_id, file_dir] : id_dirs) {
        const std::string namespace_ = doc_id.substr(0, doc_id.find(':'));
        const std::string& rel = ids[doc_id];

        const std::string* owner = nullptr;
        for (const auto& [dir, ns] : pack_namespaces) {
            if (is_under(file_dir, dir)) {
                owner = &ns;  // first ancestor in file order, matching the reference
                break;
            }
        }
        if (owner == nullptr) {
            diags.error("L002", rel, "", "no pack.yaml found in ancestor directories");
        } else if (*owner != namespace_) {
            diags.error("L002", rel, "",
                        "id namespace '" + namespace_ + "' but pack namespace is '" +
                            *owner + "'");
        }
    }

    // ---- Pass 3: L011 — every content reference resolves to a defined id.
    for (const auto& r : refs) {
        if (content_ids.find(r.resolved) == content_ids.end()) {
            diags.error("L011", r.rel_path, r.where,
                        "unresolved reference '" + r.resolved + "'");
        }
    }

    model.entity_count = content_ids.size();
    model.asset_count = asset_count;
    model.content_ref_count = refs.size();
}

}  // namespace mcc::stages
