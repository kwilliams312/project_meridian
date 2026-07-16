// tools/mcc/src/stages/pack_contract.cpp — idmap append-only lint + `mcc diff`.
// See pack_contract.h for the contract; kept in parity with validate_content.py.

#include "stages/pack_contract.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "stages/discover.h"
#include "stages/model.h"
#include "stages/parse.h"

namespace fs = std::filesystem;

namespace mcc::stages {

void scan_idmap_append_only(const idmap::IdMap& m, const std::string& rel,
                            diag::Diagnostics& diags) {
    // Index 0 is reserved (null/unset); no live or retired entry may claim it.
    for (const auto& [id, idx] : m.map) {
        if (idx == 0) {
            diags.error("L016", rel, "",
                        "map id '" + id + "' uses reserved index 0 (0 is the "
                        "null/unset id, SAD §2.4)");
        }
    }
    for (const auto& [id, idx] : m.retired) {
        if (idx == 0) {
            diags.error("L016", rel, "",
                        "retired id '" + id + "' uses reserved index 0 (SAD §2.4)");
        }
    }

    // A local index assigned to two distinct live ids — the footprint of a
    // renumber that lands one id on another's slot. std::map keeps ids sorted, so
    // the first-seen owner and the diagnostic order are deterministic.
    std::map<idmap::LocalIndex, std::string> by_index;
    for (const auto& [id, idx] : m.map) {
        auto it = by_index.find(idx);
        if (it != by_index.end()) {
            diags.error("L016", rel, "",
                        "local index " + std::to_string(idx) + " is assigned to both '" +
                            it->second + "' and '" + id +
                            "' — ids are append-only and never renumbered/reused (SAD §2.4)");
        } else {
            by_index.emplace(idx, id);
        }
    }

    // A retired index reallocated to a live id — retired indices are frozen.
    std::map<idmap::LocalIndex, std::string> retired_by_index;
    for (const auto& [id, idx] : m.retired) retired_by_index.emplace(idx, id);
    for (const auto& [id, idx] : m.map) {
        auto it = retired_by_index.find(idx);
        if (it != retired_by_index.end()) {
            diags.error("L016", rel, "",
                        "local index " + std::to_string(idx) + " for live id '" + id +
                            "' reuses a retired index (was '" + it->second +
                            "') — retired indices are frozen forever (SAD §2.4)");
        }
    }

    // An id in both map and retired is contradictory (live and retired at once).
    for (const auto& [id, idx] : m.map) {
        (void)idx;
        if (m.retired.find(id) != m.retired.end()) {
            diags.error("L016", rel, "",
                        "id '" + id + "' appears in both `map` and `retired`");
        }
    }
}

namespace {

// One pack's authoring-time snapshot for diffing: the id universe with each id's
// type + top-level field set, the idmap index per id, and the pack's declared
// compatibility_version (default 1 when absent — the baseline).
struct PackSnapshot {
    std::map<std::string, std::string> entity_type;               // id -> schema/type
    std::map<std::string, std::set<std::string>> fields;          // id -> top-level fields
    std::map<std::string, idmap::LocalIndex> id_index;            // id -> local index
    long long compatibility_version = 1;
};

// Read the top-level compatibility_version from a parsed pack file (default 1).
long long read_compat(const YAML::Node& pack_root) {
    if (pack_root && pack_root.IsMap() && pack_root["compatibility_version"]) {
        try {
            return pack_root["compatibility_version"].as<long long>();
        } catch (const std::exception&) {
            return 1;
        }
    }
    return 1;
}

// The type word of a schema envelope: "meridian/npc@2" -> "npc".
std::string type_of_schema(const std::string& schema) {
    const std::size_t slash = schema.find('/');
    if (slash == std::string::npos) return schema;
    const std::size_t at = schema.find('@', slash);
    return schema.substr(slash + 1, at == std::string::npos ? std::string::npos
                                                            : at - slash - 1);
}

// Load every namespace's idmap.lock under `content_dir` into `snap.id_index`.
void load_idmaps(const std::string& content_dir, const std::set<std::string>& namespaces,
                 PackSnapshot& snap) {
    for (const auto& ns : namespaces) {
        const fs::path lock_path = fs::path(content_dir) / ns / "idmap.lock";
        std::ifstream f(lock_path, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        idmap::IdMap m;
        std::string err;
        if (!idmap::parse(ss.str(), m, err)) continue;
        for (const auto& [id, idx] : m.map) snap.id_index[id] = idx;
    }
}

// Build a PackSnapshot from a content root, or return false if it can't be
// scanned. Runs discover + parse (not the full validate — diff must work on any
// two well-formed packs, including across a breaking edit that a stricter stage
// would reject).
bool load_snapshot(const std::string& content_dir, PackSnapshot& snap) {
    model::ContentModel model;
    if (!discover(content_dir, model)) return false;
    diag::Diagnostics throwaway;
    parse(model, throwaway);

    std::set<std::string> namespaces;
    for (const auto& pf : model.files) {
        if (!pf.parsed) continue;
        if (pf.file.kind == model::FileKind::Pack) {
            if (!pf.namespace_.empty()) namespaces.insert(pf.namespace_);
            snap.compatibility_version = read_compat(pf.root);
            continue;
        }
        if (pf.id.empty()) continue;
        snap.entity_type[pf.id] = type_of_schema(pf.schema);
        std::set<std::string> field_names;
        if (pf.root && pf.root.IsMap()) {
            for (const auto& kv : pf.root) {
                const std::string key = kv.first.as<std::string>();
                if (key == "schema" || key == "id") continue;  // envelope, not a capability
                field_names.insert(key);
            }
        }
        snap.fields[pf.id] = std::move(field_names);
    }
    load_idmaps(content_dir, namespaces, snap);
    return true;
}

// One classified change, ordered for a stable report (breaking first, then by id).
struct Change {
    bool breaking;
    std::string line;  // fully-formatted report line
};

}  // namespace

int diff_packs(const std::string& old_dir, const std::string& new_dir,
               DiagFormat format, std::ostream& out, std::ostream& err) {
    PackSnapshot old_snap;
    PackSnapshot new_snap;
    if (!load_snapshot(old_dir, old_snap)) {
        err << "mcc diff: content directory not found: " << old_dir << '\n';
        return 2;
    }
    if (!load_snapshot(new_dir, new_snap)) {
        err << "mcc diff: content directory not found: " << new_dir << '\n';
        return 2;
    }

    std::vector<Change> additive;
    std::vector<Change> breaking;

    // The id universe on each side: parsed entities ∪ idmap-mapped ids.
    std::set<std::string> old_ids;
    std::set<std::string> new_ids;
    for (const auto& [id, t] : old_snap.entity_type) old_ids.insert(id);
    for (const auto& [id, idx] : old_snap.id_index) old_ids.insert(id);
    for (const auto& [id, t] : new_snap.entity_type) new_ids.insert(id);
    for (const auto& [id, idx] : new_snap.id_index) new_ids.insert(id);

    // Removed ids (breaking) and added ids (additive).
    for (const auto& id : old_ids) {
        if (new_ids.find(id) == new_ids.end()) {
            breaking.push_back({true, "BREAKING removed id: " + id});
        }
    }
    for (const auto& id : new_ids) {
        if (old_ids.find(id) == old_ids.end()) {
            additive.push_back({false, "ADDITIVE new id: " + id});
        }
    }

    // Ids present in both: renumber, type change, field-level changes.
    for (const auto& id : old_ids) {
        if (new_ids.find(id) == new_ids.end()) continue;

        auto oi = old_snap.id_index.find(id);
        auto ni = new_snap.id_index.find(id);
        if (oi != old_snap.id_index.end() && ni != new_snap.id_index.end() &&
            oi->second != ni->second) {
            breaking.push_back({true, "BREAKING renumbered id: " + id + " (index " +
                                          std::to_string(oi->second) + " -> " +
                                          std::to_string(ni->second) + ")"});
        }

        auto ot = old_snap.entity_type.find(id);
        auto nt = new_snap.entity_type.find(id);
        if (ot != old_snap.entity_type.end() && nt != new_snap.entity_type.end() &&
            ot->second != nt->second) {
            breaking.push_back({true, "BREAKING changed id type: " + id + " (" +
                                          ot->second + " -> " + nt->second + ")"});
        }

        auto of = old_snap.fields.find(id);
        auto nf = new_snap.fields.find(id);
        if (of != old_snap.fields.end() && nf != new_snap.fields.end()) {
            for (const auto& field : of->second) {
                if (nf->second.find(field) == nf->second.end()) {
                    breaking.push_back({true, "BREAKING removed field: " + id + "." + field +
                                                  " (a removed capability)"});
                }
            }
            for (const auto& field : nf->second) {
                if (of->second.find(field) == of->second.end()) {
                    additive.push_back({false, "ADDITIVE new field: " + id + "." + field});
                }
            }
        }
    }

    const bool has_breaking = !breaking.empty();

    // Boot-gate contract: a breaking diff MUST bump compatibility_version.
    Change compat_gate{false, ""};
    bool have_compat_gate = false;
    if (has_breaking && new_snap.compatibility_version <= old_snap.compatibility_version) {
        have_compat_gate = true;
        compat_gate = {true,
                       "BREAKING compatibility_version not bumped: breaking changes require "
                       "compatibility_version > " +
                           std::to_string(old_snap.compatibility_version) + " (found " +
                           std::to_string(new_snap.compatibility_version) + ")"};
    }

    if (format == DiagFormat::Json) {
        out << "{\n  \"schema\": \"meridian/pack-diff@1\",\n";
        out << "  \"classification\": \"" << (has_breaking ? "breaking" : "additive")
            << "\",\n";
        out << "  \"old_compatibility_version\": " << old_snap.compatibility_version << ",\n";
        out << "  \"new_compatibility_version\": " << new_snap.compatibility_version << ",\n";
        out << "  \"breaking_count\": " << (breaking.size() + (have_compat_gate ? 1 : 0))
            << ",\n";
        out << "  \"additive_count\": " << additive.size() << "\n}\n";
    } else {
        out << "mcc diff: " << old_dir << " -> " << new_dir << "\n";
        out << "  classification: " << (has_breaking ? "BREAKING" : "ADDITIVE") << "\n";
        out << "  compatibility_version: " << old_snap.compatibility_version << " -> "
            << new_snap.compatibility_version << "\n";
        if (breaking.empty() && additive.empty()) {
            out << "  (no id/field changes)\n";
        }
        for (const auto& c : breaking) out << "  " << c.line << "\n";
        if (have_compat_gate) out << "  " << compat_gate.line << "\n";
        for (const auto& c : additive) out << "  " << c.line << "\n";
    }

    return has_breaking ? 1 : 0;
}

}  // namespace mcc::stages
