// tools/mcc/src/stages/index.cpp — the ID index / typed pickers / backlinks
// surface (Tools SAD §2.3, §6). See index.h for the contract.
//
// The index is a projection of the linked model: it re-derives the id universe
// the same way link.cpp does (every parsed, non-pack file with an id), reads the
// IF-9 numeric ids straight out of the LinkResult idmaps, and enriches the
// backlink index with the referencing json-path from the LinkResult edges (the
// reverse graph #119 built). No disk, no diagnostics — a pure query object.

#include "stages/index.h"

#include <algorithm>
#include <sstream>

#include "stages/check.h"
#include "stages/diagnostics.h"
#include "stages/discover.h"
#include "stages/idmap.h"
#include "stages/parse.h"
#include "stages/validate.h"

namespace mcc::stages {

namespace {

// The namespace segment of a fully-qualified id ("core:npc.x" -> "core").
std::string ns_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? std::string() : id.substr(0, colon);
}

// The grammar TYPE token of a fully-qualified id: the segment after the
// namespace colon and before the first '.' ("core:npc.kobold.miner" -> "npc").
// Matches the ref grammar's type alternation (link.cpp kRefRe) and the schema
// id/asset patterns (common.defs.yaml).
std::string type_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    const std::size_t start = colon == std::string::npos ? 0 : colon + 1;
    const std::size_t dot = id.find('.', start);
    if (dot == std::string::npos) return id.substr(start);
    return id.substr(start, dot - start);
}

// The four asset (IF-8 sidecar) grammar types, distinct from the eight content
// types. An id whose type token is one of these is an asset-registry id.
bool is_asset_type(const std::string& type) {
    return type == "art" || type == "mus" || type == "sfx" || type == "amb";
}

// Normalize a schema ref-def name to a bare grammar type token: the picker
// accepts both the schema `$defs` name ("itemRef") and the bare type ("item"),
// so an editor can pass whatever it has to hand. A trailing "Ref"/"ref" is
// stripped; everything else is returned verbatim (unknown types answer empty).
std::string normalize_ref_type(const std::string& ref_type) {
    for (const char* suffix : {"Ref", "ref"}) {
        const std::string s(suffix);
        if (ref_type.size() > s.size() &&
            ref_type.compare(ref_type.size() - s.size(), s.size(), s) == 0) {
            return ref_type.substr(0, ref_type.size() - s.size());
        }
    }
    return ref_type;
}

// Escape a string for a double-quoted JSON string literal (RFC 8259). Identical
// policy to emit_pck.cpp/emit_sql.cpp's quoter (kept local to keep the stage
// dependency-free; ids are ASCII so this is exercised mostly on file paths).
std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

}  // namespace

IdIndex IdIndex::build(const model::ContentModel& model, const LinkResult& linked) {
    IdIndex idx;

    // Flatten the per-pack idmaps into id -> numeric id for O(1) numeric lookup.
    std::map<std::string, std::uint32_t> numeric;
    for (const auto& [ns, m] : linked.idmaps) {
        for (const auto& [id, local] : m.map) {
            numeric[id] = idmap::numeric_id(m.band, local);
        }
    }

    // ---- Build one IndexEntry per id-bearing content/asset file. ------------
    // Mirrors link.cpp's id-universe walk: parsed, non-pack, id-bearing files.
    std::map<std::string, bool> ns_seen;
    for (const auto& pf : model.files) {
        if (!pf.parsed) continue;
        if (pf.file.kind == model::FileKind::Pack) continue;
        if (pf.id.empty()) continue;

        IndexEntry e;
        e.id = pf.id;
        e.type = type_of(pf.id);
        e.namespace_ = ns_of(pf.id);
        e.file = pf.file.rel_path;
        e.is_asset = pf.file.kind == model::FileKind::Asset || is_asset_type(e.type);
        const auto nit = numeric.find(pf.id);
        e.numeric_id = nit == numeric.end() ? 0 : nit->second;

        idx.by_id_[e.id] = e;
        idx.by_type_[e.type].push_back(e.id);
        ns_seen[e.namespace_] = true;
    }

    // Materialize the sorted flat list + sort each per-type id list. by_id_ is a
    // std::map so iteration is already sorted by id.
    for (const auto& [id, e] : idx.by_id_) idx.entries_.push_back(e);
    for (auto& [type, ids] : idx.by_type_) std::sort(ids.begin(), ids.end());
    for (const auto& [ns, _] : ns_seen) idx.namespaces_.push_back(ns);

    // ---- Enrich the backlink index with the referencing field path. ---------
    // LinkResult::backlinks already gives target -> {referrer ids} (deduped),
    // but find-usages wants the FIELD each usage lives at. Rebuild from the edge
    // list (from, to, file, where): one Referrer per distinct (from, where) so
    // an entity that references a target from two fields yields two jump targets.
    for (const auto& edge : linked.edges) {
        idx.backlinks_[edge.to].push_back({edge.from, edge.file, edge.where});
    }
    for (auto& [to, referrers] : idx.backlinks_) {
        std::sort(referrers.begin(), referrers.end(),
                  [](const Referrer& a, const Referrer& b) {
                      if (a.from != b.from) return a.from < b.from;
                      return a.where < b.where;
                  });
        referrers.erase(
            std::unique(referrers.begin(), referrers.end(),
                        [](const Referrer& a, const Referrer& b) {
                            return a.from == b.from && a.where == b.where;
                        }),
            referrers.end());
    }

    return idx;
}

std::vector<std::string> IdIndex::by_type(const std::string& type) const {
    const auto it = by_type_.find(type);
    return it == by_type_.end() ? std::vector<std::string>{} : it->second;
}

std::vector<std::string> IdIndex::types() const {
    std::vector<std::string> out;
    for (const auto& [type, _] : by_type_) out.push_back(type);
    return out;  // std::map order == sorted
}

std::optional<IndexEntry> IdIndex::resolve(const std::string& id) const {
    // Exact (fully-qualified) match first.
    const auto it = by_id_.find(id);
    if (it != by_id_.end()) return it->second;
    // Bare id (no "<ns>:"): default to the sole namespace when unambiguous, so an
    // editor working within one pack can pass "item.rusty_pickaxe". A multi-pack
    // corpus requires a qualified id (no guessing across packs).
    if (id.find(':') == std::string::npos && namespaces_.size() == 1) {
        const auto qit = by_id_.find(namespaces_.front() + ":" + id);
        if (qit != by_id_.end()) return qit->second;
    }
    return std::nullopt;
}

std::vector<std::string> IdIndex::pickable(const std::string& ref_type) const {
    return by_type(normalize_ref_type(ref_type));
}

std::vector<Referrer> IdIndex::backlinks(const std::string& id) const {
    // Resolve a bare id the same way resolve() does, so `mcc refs item.x` works.
    std::string key = id;
    if (backlinks_.find(key) == backlinks_.end()) {
        if (const auto e = resolve(id)) key = e->id;
    }
    const auto it = backlinks_.find(key);
    return it == backlinks_.end() ? std::vector<Referrer>{} : it->second;
}

// ---- JSON serialization -----------------------------------------------------

void render_index_json(const IdIndex& index, std::ostream& out) {
    out << "{\n";
    out << "  \"schema\": \"meridian/id-index@1\",\n";

    // namespaces present (usually one: "core").
    std::map<std::string, bool> ns_set;
    for (const auto& e : index.entries()) ns_set[e.namespace_] = true;
    out << "  \"namespaces\": [";
    {
        bool first = true;
        for (const auto& [ns, _] : ns_set) {
            out << (first ? "" : ", ") << json_quote(ns);
            first = false;
        }
    }
    out << "],\n";

    const std::vector<std::string> types = index.types();
    out << "  \"type_count\": " << types.size() << ",\n";
    out << "  \"id_count\": " << index.entries().size() << ",\n";

    // types: { "item": ["core:item.a", …], … } — the list-all-X grouping.
    out << "  \"types\": {";
    for (std::size_t i = 0; i < types.size(); ++i) {
        const std::vector<std::string> ids = index.by_type(types[i]);
        out << (i ? "," : "") << "\n    " << json_quote(types[i]) << ": [";
        for (std::size_t j = 0; j < ids.size(); ++j) {
            out << (j ? ", " : "") << json_quote(ids[j]);
        }
        out << "]";
    }
    out << (types.empty() ? "" : "\n  ") << "},\n";

    // ids: the flat resolvable universe, one object per id (sorted by id).
    out << "  \"ids\": [";
    const auto& entries = index.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const IndexEntry& e = entries[i];
        out << (i ? "," : "") << "\n    { \"id\": " << json_quote(e.id)
            << ", \"type\": " << json_quote(e.type)
            << ", \"namespace\": " << json_quote(e.namespace_)
            << ", \"numeric_id\": " << e.numeric_id
            << ", \"file\": " << json_quote(e.file)
            << ", \"is_asset\": " << (e.is_asset ? "true" : "false") << " }";
    }
    out << (entries.empty() ? "" : "\n  ") << "]\n";
    out << "}\n";
}

void render_pickable_json(const IdIndex& index, const std::string& ref_type,
                          std::ostream& out) {
    const std::vector<std::string> candidates = index.pickable(ref_type);
    out << "{\n";
    out << "  \"schema\": \"meridian/pickable@1\",\n";
    out << "  \"ref_type\": " << json_quote(ref_type) << ",\n";
    // The normalized grammar type the candidates share (for the editor's label).
    // Recomputed here (same rule as pickable) to keep the JSON self-describing.
    std::string type = ref_type;
    for (const char* suffix : {"Ref", "ref"}) {
        const std::string s(suffix);
        if (type.size() > s.size() &&
            type.compare(type.size() - s.size(), s.size(), s) == 0) {
            type = type.substr(0, type.size() - s.size());
            break;
        }
    }
    out << "  \"type\": " << json_quote(type) << ",\n";
    out << "  \"ok\": " << (candidates.empty() ? "false" : "true") << ",\n";
    out << "  \"count\": " << candidates.size() << ",\n";
    out << "  \"candidates\": [";
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto e = index.resolve(candidates[i]);
        out << (i ? "," : "") << "\n    { \"id\": " << json_quote(candidates[i])
            << ", \"numeric_id\": " << (e ? e->numeric_id : 0)
            << ", \"file\": " << json_quote(e ? e->file : std::string()) << " }";
    }
    out << (candidates.empty() ? "" : "\n  ") << "]\n";
    out << "}\n";
}

void render_backlinks_json(const IdIndex& index, const std::string& id,
                           std::ostream& out) {
    const auto entry = index.resolve(id);
    const std::vector<Referrer> referrers = index.backlinks(id);
    out << "{\n";
    out << "  \"schema\": \"meridian/backlinks@1\",\n";
    out << "  \"id\": " << json_quote(id) << ",\n";
    out << "  \"resolved\": " << (entry ? "true" : "false") << ",\n";
    if (entry) {
        out << "  \"entry\": { \"id\": " << json_quote(entry->id)
            << ", \"type\": " << json_quote(entry->type)
            << ", \"namespace\": " << json_quote(entry->namespace_)
            << ", \"numeric_id\": " << entry->numeric_id
            << ", \"file\": " << json_quote(entry->file)
            << ", \"is_asset\": " << (entry->is_asset ? "true" : "false") << " },\n";
    }
    out << "  \"referrer_count\": " << referrers.size() << ",\n";
    out << "  \"referrers\": [";
    for (std::size_t i = 0; i < referrers.size(); ++i) {
        const Referrer& r = referrers[i];
        out << (i ? "," : "") << "\n    { \"from\": " << json_quote(r.from)
            << ", \"file\": " << json_quote(r.file)
            << ", \"where\": " << json_quote(r.where) << " }";
    }
    out << (referrers.empty() ? "" : "\n  ") << "]\n";
    out << "}\n";
}

// ---- CLI orchestration ------------------------------------------------------

namespace {

// Run the read-only front half (discover+parse+validate+link, no idmap write)
// and build the index. Returns 2 if the dir can't be scanned (via `rc`), 1 if
// the front half errored (index untrustworthy), 0 otherwise. On non-zero the
// diagnostics have been rendered to `err`. On success `out_index` is filled.
int build_index_ro(const std::string& content_dir, const char* cmd, IdIndex& out_index,
                   std::ostream& err) {
    model::ContentModel model;
    if (!discover(content_dir, model)) {
        err << "mcc " << cmd << ": content directory not found: " << content_dir << '\n';
        return 2;
    }
    diag::Diagnostics diags;
    parse(model, diags);
    validate(model, diags);
    // Inspection, not a build: link read-only (allocate=false), never write
    // idmap.lock. Suppress link's own L011 (validate already emitted it) so the
    // verdict is validate's. A drift/error means the id universe can't be trusted.
    const LinkResult linked =
        link(model, content_dir, /*allocate=*/false, diags, /*emit_dangling=*/false);
    if (!diags.ok()) {
        diag::render_text(diags,
                          std::string("mcc ") + cmd + ": content has errors — index may be "
                          "incomplete; fix the errors above and re-run.",
                          err);
        return 1;
    }
    out_index = IdIndex::build(model, linked);
    return 0;
}

}  // namespace

int index_content(const std::string& content_dir, bool as_json, std::ostream& out,
                  std::ostream& err) {
    IdIndex index;
    const int rc = build_index_ro(content_dir, "index", index, err);
    if (rc != 0) return rc;

    if (as_json) {
        render_index_json(index, out);
        return 0;
    }

    // Human table: types with counts, then each id -> #numeric (file).
    out << "ID index: " << index.entries().size() << " ids across "
        << index.types().size() << " types.\n";
    for (const std::string& type : index.types()) {
        const std::vector<std::string> ids = index.by_type(type);
        out << "\n" << type << " (" << ids.size() << "):\n";
        for (const std::string& id : ids) {
            const auto e = index.resolve(id);
            out << "  " << id << " -> #" << (e ? e->numeric_id : 0) << "  "
                << (e ? e->file : std::string()) << "\n";
        }
    }
    return 0;
}

int pickable_content(const std::string& content_dir, const std::string& ref_type,
                     bool as_json, std::ostream& out, std::ostream& err) {
    IdIndex index;
    const int rc = build_index_ro(content_dir, "pickable", index, err);
    if (rc != 0) return rc;

    if (as_json) {
        render_pickable_json(index, ref_type, out);
        return 0;
    }

    const std::vector<std::string> candidates = index.pickable(ref_type);
    out << "pickable(" << ref_type << "): " << candidates.size() << " candidate(s).\n";
    for (const std::string& id : candidates) {
        const auto e = index.resolve(id);
        out << "  " << id << " -> #" << (e ? e->numeric_id : 0) << "  "
            << (e ? e->file : std::string()) << "\n";
    }
    return 0;
}

int refs_content(const std::string& content_dir, const std::string& id, bool as_json,
                 std::ostream& out, std::ostream& err) {
    IdIndex index;
    const int rc = build_index_ro(content_dir, "refs", index, err);
    if (rc != 0) return rc;

    if (as_json) {
        render_backlinks_json(index, id, out);
        return 0;
    }

    const auto entry = index.resolve(id);
    const std::vector<Referrer> referrers = index.backlinks(id);
    if (!entry) {
        out << "refs(" << id << "): id not found in the index.\n";
        return 0;
    }
    out << "refs(" << entry->id << " -> #" << entry->numeric_id << "): "
        << referrers.size() << " referrer(s).\n";
    for (const Referrer& r : referrers) {
        out << "  " << r.from << "  at " << r.where << "  (" << r.file << ")\n";
    }
    return 0;
}

}  // namespace mcc::stages
