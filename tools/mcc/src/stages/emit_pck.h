// tools/mcc/src/stages/emit_pck.h — emit-pck stage (Tools SAD §2.7, IF-5).
//
// After discover/parse -> validate -> link (refs resolved + IF-9 numeric ids
// allocated), emit-pck walks the linked content model and assembles the CLIENT
// content pack: a `pack.manifest.json` plus the pack payload the client mounts +
// verifies at boot (issue #107: load_resource_pack, then engine-pin / schema /
// hash checks, aggregate content hash held for realm validation).
//
// CONTRACT (SAD §2.7 + issue #107 client consumer):
//   * pack.manifest.json carries: schema_version (this manifest's own format),
//     pack namespace + id + semver version, content_schema_version (content
//     schema major the client checks), the PINNED godot_version (PRD R8; the
//     client refuses a mismatch), the id_band, content_hash (BLAKE3 of the pack's
//     canonical source tree — IDENTICAL to emit-sql's world_manifest hash, the
//     three-way tie, SAD §2.6), and the resource/entry list. Each entry:
//       - content/asset string id (core:npc.kobold_miner),
//       - its IF-9 numeric id (the same runtime id the SQL keys rows by),
//       - its type (npc/item/.../asset/pack),
//       - the by-ID res:// resource path (SAD §2.7: layout by ID, never by
//         source path), and
//       - a per-resource BLAKE3 (SAD §2.7 "resource list with per-resource
//         BLAKE3"; the client verifies each mounted resource).
//   * Deterministic: entries sorted by numeric id, JSON keys in a fixed order,
//     no wall-clock timestamps (built_at parameterized like emit-sql), stable
//     number formatting. Same source + same mcc => byte-identical manifest (SAD
//     §5; nondeterminism is a P0 tools bug — it breaks pack verification).
//
// M0 PACK PAYLOAD DECISION (documented, honest): a true Godot `.pck` binary can
// only be written after Godot's headless importer produces the engine resources
// (.scn/.ctex/.oggvorbisstr) — that needs the pinned Godot build (SAD §2.7
// step 1) which is not available at M0. So at M0 emit-pck produces the manifest
// (the testable IF-5 contract) plus a deterministic DIRECTORY-manifest pack
// (`pack.contents.jsonl`, one line per entry) that carries the id->resource->hash
// triples. The Godot-native `.pck` container emit (SAD §2.7 step 2: magic,
// aligned file table + blobs) is the follow-up once the pinned engine's import
// pipeline lands. The manifest schema is engine-independent and forward-stable.
//
// The stage is a pure function over already-parsed inputs (no disk writes here;
// the CLI decides where the files go). It appends diagnostics for any content it
// cannot map (e.g. an entity with no idmap entry).

#ifndef MCC_STAGES_EMIT_PCK_H
#define MCC_STAGES_EMIT_PCK_H

#include <cstdint>
#include <string>
#include <vector>

#include "stages/diagnostics.h"
#include "stages/link.h"
#include "stages/model.h"

namespace mcc::stages {

// Build metadata stamped into pack.manifest.json. `built_at` is passed in (not
// read from the wall clock) so the emit is reproducible — same default epoch as
// emit-sql; a real build overrides it via --built-at.
struct EmitPckOptions {
    std::string mcc_version = "0.0.0";               // compiler version -> mcc_version
    // Deterministic build timestamp (ISO-ish text "YYYY-MM-DD HH:MM:SS"). NOT the
    // wall clock — a fixed default keeps double-build output byte-identical.
    std::string built_at = "1970-01-01 00:00:00";
    // The pinned Godot engine version (PRD R8). The client refuses to mount a
    // pack built for a different engine. Sourced from the pack's engine.godot
    // when present; overridable so a build can pin it explicitly.
    std::string godot_version = "";
};

// One resource entry in the pack: a content/asset id, its IF-9 numeric runtime
// id, its type, the by-ID res:// path, and a per-resource BLAKE3 hash.
struct PckEntry {
    std::string id;             // fully-qualified content/asset id (core:...)
    std::uint32_t numeric_id = 0;   // IF-9 runtime id (same as the SQL keys)
    std::string type;           // npc | item | ability | ... | asset
    std::string res_path;       // res://meridian/<ns>/<...> (by ID, SAD §2.7)
    std::string resource_hash;  // 64-hex BLAKE3 of the entry's canonical payload
};

// The result of an emit-pck run for a single pack: the manifest JSON text, the
// M0 directory-manifest pack text, and the resolved metadata (for the summary
// line + tests).
struct EmitPckResult {
    std::string manifest_json;      // pack.manifest.json (the IF-5 contract)
    std::string contents_jsonl;     // M0 directory-manifest pack (one entry/line)
    // pack.data.json — the M0 CLIENT-READABLE field data the character assembler
    // (spec ②) needs but the manifest/contents (id->resource->hash only) do not
    // carry: appearance catalogs, item `visual.worn`, and dye colors. The pck's
    // per-type `tables/<type>.bin` are declarative res:// paths at M0 (no Godot
    // importer yet), so the visual FIELDS ride this dep-free JSON sidecar instead,
    // keyed by IF-9 numeric id. Deterministic (arrays sorted by numeric id, fixed
    // key order) exactly like the manifest. Read by MeridianContentDB (issue #477).
    std::string data_json;
    std::string pack_namespace;     // the pack this result describes
    std::string pack_version;       // semver from pack.yaml
    std::string content_hash;       // 64-hex BLAKE3 (== emit-sql's world_manifest)
    std::uint32_t id_band = 0;
    std::vector<PckEntry> entries;  // sorted by numeric id
    bool ok = true;
};

// Emit the IF-5 client pack for a linked content model. `linked` supplies the
// per-pack idmaps (string id -> numeric id) used for entry numeric ids and the
// id_band. Today mcc compiles a single pack; if multiple pack manifests are
// present, the FIRST (sorted by namespace) is emitted and a diagnostic notes the
// rest are deferred. Diagnostics are appended for unmappable ids (rule "E001").
// Deterministic: entries in numeric-id order, JSON keys in a fixed order.
EmitPckResult emit_pck(const model::ContentModel& model, const LinkResult& linked,
                       const EmitPckOptions& opts, diag::Diagnostics& diags);

}  // namespace mcc::stages

#endif  // MCC_STAGES_EMIT_PCK_H
