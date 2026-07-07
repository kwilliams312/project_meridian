// tools/mcc/src/stages/emit_pck.cpp — emit-pck stage (Tools SAD §2.7, IF-5).
//
// Walks the linked content model and assembles the client pack: pack.manifest.json
// (the IF-5 metadata the client mounts + verifies, issue #107) plus an M0
// directory-manifest pack payload. See emit_pck.h for the IF-5 contract and the
// M0 pack-format decision. The content_hash is the SHARED per-pack hash
// (content_hash.h) so it is byte-identical to emit-sql's world_manifest hash —
// the three-way content-hash tie (SAD §2.6).

#include "stages/emit_pck.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "hash/blake3.h"
#include "stages/content_hash.h"
#include "stages/idmap.h"

namespace mcc::stages {

namespace {

// ---- JSON scalar formatting -------------------------------------------------

// Escape a string for a double-quoted JSON string literal (RFC 8259): escape the
// quote, backslash, control chars, and forward slash is left as-is (legal
// unescaped). Deterministic; no locale.
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
                    // Other control chars: \u00XX.
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

// The namespace segment of a fully-qualified id ("core:npc.x" -> "core").
std::string ns_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? std::string() : id.substr(0, colon);
}

// The type segment: the first dotted token after the namespace ("core:art.char.x"
// -> "art", "core:npc.kobold_miner" -> "npc").
std::string type_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    const std::string local = colon == std::string::npos ? id : id.substr(colon + 1);
    const std::size_t dot = local.find('.');
    return dot == std::string::npos ? local : local.substr(0, dot);
}

// The dotted "rest" after the type ("core:art.char.kobold.miner" ->
// "char.kobold.miner"), with dots turned into path separators for a res:// path.
std::string rest_path(const std::string& id) {
    const std::size_t colon = id.find(':');
    const std::string local = colon == std::string::npos ? id : id.substr(colon + 1);
    const std::size_t dot = local.find('.');
    std::string rest = dot == std::string::npos ? std::string() : local.substr(dot + 1);
    for (char& c : rest) {
        if (c == '.') c = '/';
    }
    return rest;
}

// Map an asset's declared `class:` to the Godot engine-resource extension the
// importer would produce (SAD §2.7: source glTF/PNG/WAV become .scn/.ctex/
// .oggvorbisstr). At M0 these paths are declarative (no importer runs yet); the
// extension records the *intended* imported resource so the manifest is stable
// across the follow-up that wires real import.
std::string asset_ext_for_class(const std::string& cls) {
    if (cls == "character_model" || cls == "weapon_model" || cls == "kit_piece" ||
        cls == "vfx")
        return "scn";  // imported Godot scene
    if (cls == "icon") return "ctex";                       // compressed texture
    if (cls == "music_stem" || cls == "sfx" || cls == "ambience_bed")
        return "oggvorbisstr";  // imported Vorbis stream
    return "res";               // unknown class: generic resource
}

// The by-ID res:// resource path (SAD §2.7: "layout is by ID, never by source
// path"). Assets get their imported-resource extension; content entities get the
// per-type client table blob (res://meridian/<ns>/tables/<type>.bin) since their
// client-visible data ships as a FlatBuffers table, not a per-entity file.
std::string res_path_for(const std::string& id, model::FileKind kind,
                          const std::string& asset_class) {
    const std::string ns = ns_of(id);
    const std::string type = type_of(id);
    if (kind == model::FileKind::Asset) {
        const std::string rest = rest_path(id);
        const std::string ext = asset_ext_for_class(asset_class);
        return "res://meridian/" + ns + "/" + type + "/" + rest + "." + ext;
    }
    // Content entity: client-visible rows live in the per-type table blob.
    return "res://meridian/" + ns + "/tables/" + type + ".bin";
}

// Per-resource BLAKE3 (SAD §2.7 "resource list with per-resource BLAKE3"): the
// client verifies each mounted resource. At M0 the imported binary does not
// exist yet, so the per-resource hash is taken over the entry's CANONICAL SOURCE
// payload — framed identically to the pack content_hash (rel_path\0canon\0) so
// it is deterministic and ties to exactly what was compiled. This is the same
// verification role the imported-blob hash will play in the follow-up.
std::string resource_hash_of(const model::ParsedFile& pf) {
    hash::Blake3 h;
    const std::string& rp = pf.file.rel_path;
    YAML::Emitter em;
    em << pf.root;
    const std::string canon = em.c_str() ? em.c_str() : "";
    h.update(rp.data(), rp.size());
    h.update("\0", 1);
    h.update(canon.data(), canon.size());
    h.update("\0", 1);
    return h.hex();
}

// yaml-cpp accessor: is `key` a present, non-null scalar/map under `n`?
bool has(const YAML::Node& n, const char* key) {
    return n.IsMap() && n[key] && !n[key].IsNull();
}
std::string as_str(const YAML::Node& n) { return n.Scalar(); }

// Defensive int read (content is already validated; matches emit_sql's as_int).
int as_int(const YAML::Node& n, int fallback) {
    try {
        return n.as<int>();
    } catch (...) {
        return fallback;
    }
}

}  // namespace

EmitPckResult emit_pck(const model::ContentModel& model, const LinkResult& linked,
                       const EmitPckOptions& opts, diag::Diagnostics& diags) {
    EmitPckResult result;

    // ---- Locate the pack manifest(s). mcc compiles a single pack today; emit
    // the FIRST (sorted by namespace) and note any others as deferred.
    std::vector<const model::ParsedFile*> packs;
    for (const auto& pf : model.files) {
        if (pf.parsed && pf.file.kind == model::FileKind::Pack) packs.push_back(&pf);
    }
    std::sort(packs.begin(), packs.end(),
              [](const model::ParsedFile* a, const model::ParsedFile* b) {
                  return a->namespace_ < b->namespace_;
              });
    if (packs.empty()) {
        diags.error("E002", "content/", "",
                    "emit-pck: no pack.yaml found — cannot emit pack.manifest.json");
        result.ok = false;
        return result;
    }
    const model::ParsedFile& pack = *packs[0];
    const std::string ns = pack.namespace_;
    for (std::size_t i = 1; i < packs.size(); ++i) {
        diags.info("I100", packs[i]->file.rel_path, "",
                   "emit-pck: pack '" + packs[i]->namespace_ +
                       "' deferred (single-pack emit at M0)");
    }

    result.pack_namespace = ns;
    result.pack_version = has(pack.root, "version") ? as_str(pack.root["version"]) : "0.0.0";
    const std::uint32_t schema_version =
        has(pack.root, "content_schema_version")
            ? static_cast<std::uint32_t>(as_int(pack.root["content_schema_version"], 1))
            : 1;

    // Pinned Godot version: explicit option wins, else the pack's engine.godot.
    std::string godot_version = opts.godot_version;
    if (godot_version.empty() && has(pack.root, "engine") &&
        has(pack.root["engine"], "godot")) {
        godot_version = as_str(pack.root["engine"]["godot"]);
    }

    // ---- content_hash: the SHARED per-pack hash (byte-identical to emit-sql's
    // world_manifest content_hash — the three-way tie, SAD §2.6).
    const std::map<std::string, std::string> pack_hash = compute_pack_hashes(model);
    const auto hit = pack_hash.find(ns);
    result.content_hash =
        hit != pack_hash.end() ? hit->second : std::string(hash::kBlake3HexLen, '0');

    // ---- id_band from the pack's idmap.
    const auto mit = linked.idmaps.find(ns);
    result.id_band = mit != linked.idmaps.end() ? mit->second.band : 0;

    // ---- Build the entry list: every content entity + asset sidecar in this
    // namespace, with its IF-9 numeric id, res:// path, and per-resource hash.
    for (const auto& pf : model.files) {
        if (!pf.parsed) continue;
        if (pf.file.kind != model::FileKind::Content && pf.file.kind != model::FileKind::Asset)
            continue;
        if (pf.id.empty() || ns_of(pf.id) != ns) continue;

        PckEntry e;
        e.id = pf.id;
        e.type = pf.file.kind == model::FileKind::Asset ? "asset" : pf.file.file_type;
        // Resolve the IF-9 numeric id from the pack's idmap.
        std::uint32_t numeric = 0;
        if (mit != linked.idmaps.end()) {
            const auto iit = mit->second.map.find(pf.id);
            if (iit != mit->second.map.end()) {
                numeric = idmap::numeric_id(mit->second.band, iit->second);
            }
        }
        if (numeric == 0) {
            diags.error("E001", pf.file.rel_path, "$.id",
                        "emit-pck: entity '" + pf.id +
                            "' has no IF-9 numeric id (idmap.lock out of date?)");
            result.ok = false;
        }
        e.numeric_id = numeric;
        const std::string asset_class =
            (pf.file.kind == model::FileKind::Asset && has(pf.root, "class"))
                ? as_str(pf.root["class"])
                : std::string();
        e.res_path = res_path_for(pf.id, pf.file.kind, asset_class);
        e.resource_hash = resource_hash_of(pf);
        result.entries.push_back(std::move(e));
    }

    // Deterministic: entries sorted by numeric id (ties broken by string id, so
    // an unmapped id=0 entry still has a stable position).
    std::sort(result.entries.begin(), result.entries.end(),
              [](const PckEntry& a, const PckEntry& b) {
                  if (a.numeric_id != b.numeric_id) return a.numeric_id < b.numeric_id;
                  return a.id < b.id;
              });

    // ---- Render pack.manifest.json (fixed key order; 2-space indent). ----------
    std::ostringstream j;
    j << "{\n";
    j << "  \"schema\": \"meridian/pack-manifest@1\",\n";
    j << "  \"pack\": " << json_quote(ns) << ",\n";
    j << "  \"namespace\": " << json_quote(ns) << ",\n";
    j << "  \"version\": " << json_quote(result.pack_version) << ",\n";
    j << "  \"content_schema_version\": " << schema_version << ",\n";
    j << "  \"godot_version\": " << json_quote(godot_version) << ",\n";
    j << "  \"id_band\": " << result.id_band << ",\n";
    j << "  \"content_hash\": " << json_quote(result.content_hash) << ",\n";
    j << "  \"mcc_version\": " << json_quote(opts.mcc_version) << ",\n";
    j << "  \"built_at\": " << json_quote(opts.built_at) << ",\n";
    j << "  \"entry_count\": " << result.entries.size() << ",\n";
    j << "  \"entries\": [";
    for (std::size_t i = 0; i < result.entries.size(); ++i) {
        const PckEntry& e = result.entries[i];
        j << (i == 0 ? "\n" : ",\n");
        j << "    { \"id\": " << json_quote(e.id)
          << ", \"numeric_id\": " << e.numeric_id
          << ", \"type\": " << json_quote(e.type)
          << ", \"resource\": " << json_quote(e.res_path)
          << ", \"hash\": " << json_quote(e.resource_hash) << " }";
    }
    j << (result.entries.empty() ? "" : "\n  ") << "]\n";
    j << "}\n";
    result.manifest_json = j.str();

    // ---- Render the M0 directory-manifest pack (one entry per line: the
    // id -> resource -> hash triple). This is the M0 stand-in for the Godot .pck
    // container; it carries exactly the id/resource/hash the client would mount,
    // in the same deterministic order as the manifest entry list.
    std::ostringstream p;
    for (const PckEntry& e : result.entries) {
        p << "{\"id\":" << json_quote(e.id) << ",\"numeric_id\":" << e.numeric_id
          << ",\"resource\":" << json_quote(e.res_path)
          << ",\"hash\":" << json_quote(e.resource_hash) << "}\n";
    }
    result.contents_jsonl = p.str();

    if (!diags.ok()) result.ok = false;
    return result;
}

}  // namespace mcc::stages
