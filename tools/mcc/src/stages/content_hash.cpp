// tools/mcc/src/stages/content_hash.cpp — per-pack content hash (IF-4 + IF-5).
//
// The single source of truth for the pack content_hash shared by emit-sql
// (world_manifest) and emit-pck (pack.manifest.json). See content_hash.h for the
// canonical-form contract; this file is the ONLY place that framing is defined,
// so the two emitted artifacts can never disagree.

#include "stages/content_hash.h"

#include <algorithm>
#include <vector>

#include "hash/blake3.h"
#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/parse.h"

namespace mcc::stages {

namespace {

// The namespace segment of a fully-qualified id ("core:npc.x" -> "core").
std::string ns_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? std::string() : id.substr(0, colon);
}

}  // namespace

std::map<std::string, std::string> compute_pack_hashes(const model::ContentModel& model) {
    // Gather every parseable file, then iterate in sorted rel_path order so the
    // digest is a pure function of the source tree, platform-independent.
    std::vector<const model::ParsedFile*> all_files;
    for (const auto& pf : model.files) {
        if (!pf.parsed) continue;
        all_files.push_back(&pf);
    }
    std::sort(all_files.begin(), all_files.end(),
              [](const model::ParsedFile* a, const model::ParsedFile* b) {
                  return a->file.rel_path < b->file.rel_path;
              });

    // ns -> running hash. Each file contributes "rel_path\0<canonical-yaml>\0".
    std::map<std::string, hash::Blake3> running;
    for (const model::ParsedFile* pf : all_files) {
        // Pack manifests carry the namespace directly; content/asset ids carry it
        // in the id prefix.
        std::string ns;
        if (pf->file.kind == model::FileKind::Pack) ns = pf->namespace_;
        else ns = ns_of(pf->id);
        if (ns.empty()) continue;

        // Re-serialize the parsed YAML canonically for a stable, editor-agnostic
        // hash input (whitespace/comment-insensitive; matches "canonical source
        // tree" in the SAD). Emitters use the parsed model, so hashing the parsed
        // form ties the hash to exactly what was compiled.
        YAML::Emitter em;
        em << pf->root;
        const std::string canon = em.c_str() ? em.c_str() : "";
        hash::Blake3& h = running[ns];
        const std::string& rp = pf->file.rel_path;
        h.update(rp.data(), rp.size());
        h.update("\0", 1);
        h.update(canon.data(), canon.size());
        h.update("\0", 1);
    }

    std::map<std::string, std::string> out;
    for (const auto& [ns, h] : running) out[ns] = h.hex();
    return out;
}

int content_hash_report(const std::string& content_dir, bool as_json, std::ostream& out,
                        std::ostream& err) {
    model::ContentModel model;
    if (!discover(content_dir, model)) {
        err << "mcc content-hash: content directory not found: " << content_dir << '\n';
        return 2;
    }
    diag::Diagnostics throwaway;  // hash the tree as parsed; diagnostics are check's job
    parse(model, throwaway);

    const std::map<std::string, std::string> hashes = compute_pack_hashes(model);
    if (as_json) {
        out << "{\n  \"schema\": \"meridian/content-hash@1\",\n  \"packs\": {";
        bool first = true;
        for (const auto& [ns, hex] : hashes) {
            out << (first ? "\n" : ",\n") << "    \"" << ns << "\": \"" << hex << "\"";
            first = false;
        }
        out << (first ? "" : "\n") << "  }\n}\n";
    } else {
        for (const auto& [ns, hex] : hashes) out << ns << "  " << hex << "\n";
    }
    return 0;
}

}  // namespace mcc::stages
