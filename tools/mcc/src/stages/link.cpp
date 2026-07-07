// tools/mcc/src/stages/link.cpp — link stage (Tools SAD §2.3).
//
// Resolves the `*Ref` reference graph, builds the backlink index, and drives the
// IF-9 idmap allocator per pack (SAD §2.3, §2.4). The reference grammar and the
// scalar-walk mirror validate.cpp deliberately: link resolves the SAME edges the
// validate stage checks for L011, so the two agree on what "a reference" is.

#include "stages/link.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace mcc::stages {

namespace {

// Content reference grammar — identical to validate.cpp's kRefRe (SAD §2.3: link
// resolves the same edges validate checks). An optional "<namespace>:" prefix
// then "<type>.<segment>(.<segment>)*" over the eight content types.
const std::regex kRefRe(
    R"(^(?:([a-z][a-z0-9_]{1,31}):)?((npc|item|quest|ability|loot|vendor|spawn|zone)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)$)");

// Resolve a bare ref to a fully-qualified id: refs without a "<ns>:" prefix
// default to the owning entity's namespace (schema README §References).
std::string resolve(const std::string& ref, const std::string& namespace_) {
    return ref.find(':') != std::string::npos ? ref : namespace_ + ":" + ref;
}

// Walk every scalar string in a YAML tree, yielding (json-path, value), skipping
// the top-level `id` (the definition, not a reference) — matches validate.cpp.
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

// The namespace segment of a fully-qualified id ("core:npc.x" -> "core").
std::string ns_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? std::string() : id.substr(0, colon);
}

// Read /content/<namespace>/idmap.lock into `m` if it exists. A missing file is
// not an error (first allocation). A malformed file is reported as L015.
void load_lock(const std::string& content_dir, const std::string& namespace_,
               idmap::IdMap& m, diag::Diagnostics& diags) {
    const fs::path lock_path = fs::path(content_dir) / namespace_ / "idmap.lock";
    const std::string rel = (fs::path(namespace_) / "idmap.lock").generic_string();
    std::error_code ec;
    if (!fs::exists(lock_path, ec)) return;  // first allocation — start clean

    std::ifstream in(lock_path);
    if (!in) {
        diags.error("L015", rel, "", "idmap.lock exists but could not be read");
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string err;
    if (!idmap::parse(ss.str(), m, err)) {
        diags.error("L015", rel, "", err);
    }
}

}  // namespace

LinkResult link(const model::ContentModel& model, const std::string& content_dir,
                bool allocate, diag::Diagnostics& diags, bool emit_dangling) {
    LinkResult result;

    // ---- Pass 1: collect the id universe + every reference edge. ------------
    // `defined` is the set of content ids that can be a reference target (L011).
    // `all_ids` (content + assets) is the id universe the idmap allocates over,
    // grouped by namespace (SAD §2.4: assets get IF-9 entries too, §2.4 line 256).
    std::unordered_set<std::string> defined;                 // content ids (ref targets)
    std::map<std::string, std::vector<std::string>> pack_ids;  // ns -> all ids (content+asset)

    struct PendingRef {
        std::string from, to, file, where;
    };
    std::vector<PendingRef> refs;

    for (const auto& pf : model.files) {
        if (!pf.parsed) continue;
        if (pf.file.kind == model::FileKind::Pack) continue;
        if (pf.id.empty()) continue;

        const std::string id_ns = ns_of(pf.id);
        pack_ids[id_ns].push_back(pf.id);

        if (pf.file.kind == model::FileKind::Asset) {
            // Assets are allocated an IF-9 id but define no L011 content refs
            // (their internal refs are metadata) — parity with validate.cpp.
            continue;
        }
        defined.insert(pf.id);

        std::vector<std::pair<std::string, std::string>> strings;
        walk_strings(pf.root, "$", /*top_level=*/true, strings);
        for (const auto& [loc, value] : strings) {
            std::smatch mt;
            if (std::regex_match(value, mt, kRefRe)) {
                const std::string ns = mt[1].matched ? mt[1].str() : id_ns;
                refs.push_back({pf.id, resolve(mt[2].str(), ns), pf.file.rel_path, loc});
            }
        }
    }

    // ---- Pass 2: resolve edges, report danglers (L011), build backlinks. ----
    for (const auto& r : refs) {
        if (defined.find(r.to) == defined.end()) {
            // Dangling reference — target missing (SAD §2.3). Same rule id +
            // message shape as the validate stage's L011 for a consistent verdict.
            // Suppressed in the full DAG (validate already emitted L011); still
            // counted + excluded from the graph regardless of `emit_dangling`.
            if (emit_dangling) {
                diags.error("L011", r.file, r.where, "unresolved reference '" + r.to + "'");
            }
            ++result.dangling_count;
            continue;
        }
        result.edges.push_back({r.from, r.to, r.file, r.where});
        result.backlinks[r.to].push_back(r.from);
    }
    // Sort + de-dup each backlink list for a stable, deterministic index (a form
    // may reference the same target twice; find-usages wants each referrer once).
    for (auto& [to, referrers] : result.backlinks) {
        std::sort(referrers.begin(), referrers.end());
        referrers.erase(std::unique(referrers.begin(), referrers.end()), referrers.end());
    }

    // ---- Pass 3: IF-9 idmap allocation, per pack (SAD §2.4). ----------------
    for (auto& [namespace_, ids] : pack_ids) {
        idmap::IdMap m;
        m.namespace_ = namespace_;
        load_lock(content_dir, namespace_, m, diags);
        // A lock's namespace must match the pack it lives under (SAD §2.4).
        if (!m.namespace_.empty() && m.namespace_ != namespace_) {
            const std::string rel = (fs::path(namespace_) / "idmap.lock").generic_string();
            diags.error("L015", rel, "",
                        "idmap.lock namespace '" + m.namespace_ + "' != pack namespace '" +
                            namespace_ + "'");
        }
        m.namespace_ = namespace_;

        const idmap::AllocationResult alloc = idmap::allocate(m, ids);

        if (alloc.overflow) {
            const std::string rel = (fs::path(namespace_) / "idmap.lock").generic_string();
            diags.error("L015", rel, "",
                        "id band exhausted for namespace '" + namespace_ +
                            "' (> " + std::to_string(idmap::kMaxLocalIndex) + " local indices)");
        }

        // Read-only (CI) contract: if new ids were allocated but we are not
        // permitted to write, that is idmap drift — fail with L015 (SAD §2.3).
        if (!allocate && alloc.changed) {
            const std::string rel = (fs::path(namespace_) / "idmap.lock").generic_string();
            for (const auto& id : alloc.newly_allocated) {
                diags.error("L015", rel, "",
                            "unmapped id '" + id +
                                "' — run 'mcc build --allocate-ids' to update idmap.lock");
            }
            for (const auto& id : alloc.retired) {
                diags.error("L015", rel, "",
                            "mapped id '" + id +
                                "' has no entity — it must be retired via 'mcc build --allocate-ids'");
            }
        }

        // Write the updated lock when allocation is permitted and something
        // changed (editor-invoked builds default to --allocate-ids, SAD §2.3).
        if (allocate && alloc.changed && !alloc.overflow) {
            const fs::path lock_path = fs::path(content_dir) / namespace_ / "idmap.lock";
            std::error_code ec;
            fs::create_directories(lock_path.parent_path(), ec);
            std::ofstream out(lock_path, std::ios::binary | std::ios::trunc);
            if (out) {
                out << idmap::serialize(m);
            } else {
                const std::string rel =
                    (fs::path(namespace_) / "idmap.lock").generic_string();
                diags.error("L015", rel, "", "could not write idmap.lock");
            }
        }

        result.idmaps.emplace(namespace_, std::move(m));
    }

    return result;
}

}  // namespace mcc::stages
