// tools/mcc/src/stages/discover.cpp — discover stage implementation.

#include "stages/discover.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace mcc::stages {

namespace {

// The content types, in schema-README order. The first eight are the original
// server-backed entities; `appearance` (spec §5.1 catalogs) and `dye` (spec §6)
// are the CLIENT-only visual types added by contract ① — they classify + flow
// into the client pck but emit NO world.sql (the server never reads visuals,
// spec §8). `attribute` (pack-contract spec §2.2) is the kernel-blessed base
// stat vocabulary — a rules-data catalog that classifies + flows into the pck
// but emits no world.sql this round (kernel formulas are sub-project 2).
// `equip_type` (pack-contract spec §2.1) is the armor/weapon-type CATALOG
// (rules-data referenced by items and, in sub-project 2, class proficiencies).
// `talent` + `talent_tree` (pack-contract spec §2.5) are the talent catalog and
// its tiered row-unlock tree — rules-data referenced by a class's talent_tree
// (sub-project 2). Like the other non-server-emitting types they classify + flow
// into the client pck but emit NO world.sql this round. A registered type with
// zero content entities is fine (no seed talents ship in sub-project 1).
// Mirrors validate_content.py's CONTENT_TYPES (the reference validator).
constexpr std::array<std::string_view, 15> kContentTypes = {
    "npc",        "item",       "quest",  "ability",     "loot",
    "vendor",     "spawn",      "zone",   "appearance",  "dye",
    "attribute",  "equip_type", "race",   "talent",      "talent_tree"};

// Split `s` on '.' into its dot-separated parts.
std::vector<std::string> split_dots(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// Normalize a path to forward slashes for cross-platform-stable diagnostics.
std::string to_forward_slashes(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

}  // namespace

bool is_content_type(const std::string& t) {
    return std::find(kContentTypes.begin(), kContentTypes.end(), t) != kContentTypes.end();
}

model::FileKind classify(const std::string& filename, std::string& file_type_out) {
    file_type_out.clear();
    if (filename == "pack.yaml") {
        file_type_out = "pack";
        return model::FileKind::Pack;
    }
    // Mirrors validate_content.py::file_type — exactly three dot-parts, the last
    // literally "yaml", the middle a content type or "asset".
    const std::vector<std::string> parts = split_dots(filename);
    if (parts.size() == 3 && parts[2] == "yaml") {
        const std::string& t = parts[1];
        if (is_content_type(t)) {
            file_type_out = t;
            return model::FileKind::Content;
        }
        if (t == "asset") {
            file_type_out = "asset";
            return model::FileKind::Asset;
        }
    }
    return model::FileKind::Unknown;
}

bool discover(const std::string& content_dir, model::ContentModel& model) {
    std::error_code ec;
    const fs::path content_path = fs::absolute(fs::path(content_dir), ec);
    if (ec || !fs::exists(content_path) || !fs::is_directory(content_path)) {
        return false;
    }

    model.content_dir = content_path.generic_string();
    model.root_dir = content_path.parent_path().generic_string();

    // Collect every *.yaml under content_dir, then sort by absolute path so
    // iteration order is deterministic and matches the reference validator's
    // sorted rglob (SAD §9.4).
    std::vector<model::DiscoveredFile> found;
    for (fs::recursive_directory_iterator it(content_path, ec), end; it != end && !ec;
         it.increment(ec)) {
        const fs::path& p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (p.extension() != ".yaml") continue;
        // *.render.yaml are auxiliary strudel-render manifests (epic #400 / issue
        // #410), not content entities — skip them, mirroring validate_content.py's
        // discovery (which excludes `*.render.yaml` from its rglob). Keeps the
        // mcc-parity file_count identical to the reference validator.
        // *.prompts.yaml are the Meshy intake CLI's AI-provenance companions
        // (spec ④ §7.2, story #505) — same auxiliary shape, same exclusion.
        const std::string fname = p.filename().string();
        if (fname.ends_with(".render.yaml")) continue;
        if (fname.ends_with(".prompts.yaml")) continue;

        model::DiscoveredFile df;
        df.abs_path = p.generic_string();
        // rel_path is repo-root-relative (root = content's parent), matching
        // validate_content.py's rel() which is relative to content_dir.parent.
        df.rel_path = to_forward_slashes(fs::relative(p, model.root_dir, ec).generic_string());
        df.kind = classify(p.filename().string(), df.file_type);
        found.push_back(std::move(df));
    }

    std::sort(found.begin(), found.end(),
              [](const model::DiscoveredFile& a, const model::DiscoveredFile& b) {
                  return a.abs_path < b.abs_path;
              });

    model.file_count = found.size();
    for (auto& df : found) {
        model::ParsedFile pf;
        pf.file = std::move(df);
        model.files.push_back(std::move(pf));
    }
    return true;
}

}  // namespace mcc::stages
