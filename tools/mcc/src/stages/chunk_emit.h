// tools/mcc/src/stages/chunk_emit.h — chunk-emit stage (Tools SAD §3, IF-6).
//
// The v0 slice of the REAL mcc chunk stage, in *procedural fixture* mode
// (story #553, first domino of epic #22). It emits a small N×N Zone-01 fixture
// pack that is byte-shaped EXACTLY like production chunk output — it goes through
// the real schemas (schema/chunk/chunk.fbs `ServerChunk` + Heightfield, and
// schema/chunk/chunk-manifest.schema.yaml) and the real vendored BLAKE3
// (hash/blake3.h), so client stories A–E can build + test against a
// contract-accurate pack without waiting on Forge v1 (#26) / the chunk-exporter
// (#315) / the full Zone-01 greybox (#17).
//
// Later this stage grows a SECOND input path (consume a Forge/#315 export). This
// procedural mode is NOT a throwaway hand-authored pack — it is the v0 emit path,
// so the fixture can never rot or diverge from what mcc actually produces.
//
// CONTRACT (Tools SAD §3.1–§3.4 + the A-08 walk amendments C2/C3/C4/C5):
//   For a zone (default core:zone.zone01) and an N×N grid (default 3×3) it emits:
//     * <zone>.chunks.json — the IF-6 manifest (chunk-manifest.schema.yaml):
//       a non-zero origin, chunk_size_m (128), inclusive grid bounds spanning
//       negative indices (Zone-01 spawns at x ≈ −300, §3.1), far_ring, and one
//       sparse entry per chunk carrying C2 asset-ID refs (scene / proxy / server),
//       an aabb, a load-order priority, shared-asset deps, and the per-chunk
//       BLAKE3-256 (`blake3:<64hex>`) over BOTH payloads.
//     * per chunk <cx>_<cz>.chunk.bin — a REAL `ServerChunk` FlatBuffer whose
//       129×129 f32 Heightfield is deliberately NON-FLAT (a ramp + shallow bowl,
//       a pure function of zone-local world coords so shared edges join exactly)
//       — the contract-critical artifact the client reads the heightfield from
//       (Q1(a)); a flat-vs-sloped bug in HeightfieldWorldQuery / visuals is
//       catchable against it.
//     * per chunk <cx>_<cz>.tscn + <cx>_<cz>.proxy.tscn — VALID Godot TEXT scenes
//       the streamer can instance directly. They carry the `.tscn` extension (not
//       `.scn`) so Godot's ResourceLoader routes them to the TEXT loader — a text
//       payload under a `.scn` name is rejected by the BINARY loader
//       ("Unrecognized binary resource file", #579). (The real content pipeline —
//       Forge/#315 — ships binary `.scn` the loader reads directly; this is a
//       FIXTURE-correctness choice.)
//       Each scene holds a MeshInstance3D with an `ArrayMesh` BAKED FROM THE
//       CHUNK'S OWN HEIGHTFIELD (#875): 129×129 vertices (the samples 1:1), per-
//       vertex normals, UVs spanning the chunk 0..1, and a StandardMaterial3D
//       slot keyed to the shared terrain-ground dep id. The node is translated to
//       the chunk's zone-local XZ corner at y=0, so a vertex's Y *is* its world
//       height and neighbours tile exactly along the shared edge. The proxy is the
//       same surface decimated 4× per axis (33×33). Before #875 these scenes were a
//       single BoxMesh per chunk — mounting a zone drew giant boxes, not terrain.
//
// GODOT MESH ENCODING (the one place mcc is coupled to the engine): Godot
//   serialises an ArrayMesh surface as base64 PackedByteArray blobs whose layout —
//   tight f32 positions, then OCTAHEDRON-encoded unorm16 normal+tangent pairs, a
//   separate UV attribute buffer, u16 indices — is fixed by the engine's mesh
//   array-format VERSION (the high bits of the surface `format` word). mcc is C++
//   and cannot link Godot, so it writes those bytes itself. The emitted `format`
//   is asserted in the tests against what the PINNED engine (client/ENGINE_VERSION,
//   Godot 4.7-stable) itself produces for the same surface; that assertion is the
//   tripwire if an engine bump changes the layout. Re-verify against the new engine
//   at each milestone pin bump rather than hand-editing the constant.
//     * <zone>.assets.json — the IF-8 asset-ID table so the C2 refs resolve
//       (id → local index → IF-9 numeric id, band 0, allocated lexicographically).
//     * a client pack (pack.manifest.json + M0 pack.contents.jsonl, the same
//       `meridian/pack-manifest@1` shape emit-pck #121 produces) that INCLUDES
//       the server `.chunk.bin` per chunk (Q1(a)) alongside the scene/proxy/deps.
//
// PER-CHUNK HASH FRAMING (deterministic, documented): the manifest `hash` is
//   BLAKE3( "server\0" <chunk.bin bytes> "\0" "scene\0" <.tscn bytes> "\0"
//           "proxy\0"  <.proxy.tscn bytes> "\0" )
// i.e. it covers BOTH the server payload and the client scene payloads, framed
// with a label + NUL after each so no concatenation collision is possible. Any
// change to any payload invalidates the entry (walk C5/C8). Rendered `blake3:<hex>`.
//
// DETERMINISM: identical options ⇒ identical bytes. All geometry is pure integer/
// float arithmetic (no wall-clock, no map iteration in output order). The only
// libm calls are sqrt/fabs in the mesh normal path — both IEEE-754
// correctly-rounded and therefore bit-identical across platforms, unlike the
// transcendentals (sin/exp), which stay banned. Nondeterminism here is a P0 — it
// breaks pack verification.

#ifndef MCC_STAGES_CHUNK_EMIT_H
#define MCC_STAGES_CHUNK_EMIT_H

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "stages/check.h"        // DiagFormat
#include "stages/diagnostics.h"

namespace mcc::stages {

// Options for a procedural chunk-emit run. Defaults produce the canonical 3×3
// Zone-01 fixture with a non-zero, negative-index origin.
struct ChunkEmitOptions {
    std::string zone = "core:zone.zone01";  // owning zone content id (IF-6 manifest `zone`)
    int grid = 3;                            // N for the N×N grid (>=1)
    // Zone-local grid origin in metres (non-zero — Zone-01 sits at x ≈ −300, §3.1).
    // Placed so the N×N grid straddles negative chunk indices (contract-accurate).
    double origin_x = -384.0;
    double origin_z = -384.0;
    int chunk_size_m = 128;                  // v1 baseline (§3.2); a manifest FIELD (TS-3)
    int far_ring = 6;                        // clipmap unload ring (§3.2 default 6)
    std::string mcc_version = "0.0.0";       // stamped into pack.manifest.json
    // Deterministic build timestamp (never the wall clock) — same epoch idiom as
    // emit-sql / emit-pck so double-build output is byte-identical.
    std::string built_at = "1970-01-01 00:00:00";
    std::string godot_version = "";          // engine pin recorded in the pack manifest
};

// One IF-8 asset-table / pack entry: a logical resource ref, its band-0 local
// index + IF-9 numeric id, its kind, the by-ID res:// path, and a per-resource
// BLAKE3 (bare 64-hex, the same per-resource verification role emit-pck uses).
struct ChunkAsset {
    std::string id;                 // C2 asset-ID / ID-derived ref (core:chunk.zone01.n1_n1.server)
    std::uint32_t local_index = 0;  // band-0 local index (append-only, lexicographic)
    std::uint32_t numeric_id = 0;   // IF-9 numeric runtime id (band*2^20 + local_index)
    std::string type;               // chunk_server | chunk_scene | chunk_proxy | asset
    std::string res_path;           // res://meridian/<ns>/chunks/<zone>/<tag>.<ext>
    std::string hash;               // 64-hex BLAKE3 of the payload (id-derived for depless logical assets)
};

// The fully-generated record for one chunk (in zone-local metres / grid indices).
struct ChunkRecord {
    int cx = 0;
    int cz = 0;
    std::string tag;         // filename/id-safe coord tag ("n1_n1"; 'n' = negative)
    std::string scene_ref;   // C2 client scene ref
    std::string proxy_ref;   // C2 LOD proxy ref
    std::string server_ref;  // C2 server payload ref
    std::vector<std::string> deps;  // shared IF-8 asset ids (sorted, de-duped)
    int priority = 0;        // load-order hint (lower first; 0 = grid centre)

    std::string chunk_bin;   // ServerChunk FlatBuffer bytes (<cx>_<cz>.chunk.bin)
    std::string scn;         // client scene text (<cx>_<cz>.tscn)
    std::string proxy_scn;   // proxy scene text (<cx>_<cz>.proxy.tscn)

    std::string entry_hash;  // "blake3:<64hex>" over BOTH payloads (manifest `hash`)

    // Zone-local AABB (metres). Y bounds are the chunk's own heightfield min/max.
    float min_x = 0, min_y = 0, min_z = 0;
    float max_x = 0, max_y = 0, max_z = 0;
};

// The result of a chunk-emit run: the manifest + asset-table + pack texts and the
// per-chunk / per-asset records (for the CLI to write and the tests to inspect).
struct ChunkEmitResult {
    std::string zone;            // full zone id (core:zone.zone01)
    std::string zone_bare;       // trailing segment ("zone01") for filenames/paths
    std::string ns;              // pack namespace ("core")
    std::string manifest_json;   // <zone>.chunks.json (IF-6 manifest)
    std::string assets_json;     // <zone>.assets.json (IF-8 asset-ID table)
    std::string pack_manifest_json;   // pack.manifest.json (IF-5, meridian/pack-manifest@1)
    std::string pack_contents_jsonl;  // M0 directory pack (one entry per line)
    std::string content_hash;    // 64-hex BLAKE3 pack content hash (in pack.manifest.json)
    std::vector<ChunkRecord> chunks;  // row-major by (cz, cx)
    std::vector<ChunkAsset> assets;   // sorted by numeric id (== local index order)
    bool ok = true;
};

// Generate the procedural fixture. Pure over `opts` (no disk I/O; the CLI wrapper
// decides where the bytes land). Appends diagnostics for a bad option (e.g. a
// grid < 1 or a malformed zone id) and sets ok=false.
ChunkEmitResult chunk_emit(const ChunkEmitOptions& opts, diag::Diagnostics& diags);

// CLI wrapper: run chunk_emit then, when `out_dir` is non-empty, write the whole
// fixture pack under <out_dir>/meridian/<ns>/chunks/<zone>/ (manifest, asset
// table, pack manifest + M0 pack, and every per-chunk .chunk.bin / .tscn /
// .proxy.tscn). With an empty `out_dir` the manifest goes to `out` (diagnostics to
// `err`). Returns 0 on success, 1 on any error, 2 when `out_dir` cannot be written.
int chunk_emit_run(const ChunkEmitOptions& opts, const std::string& out_dir,
                   DiagFormat format, std::ostream& out, std::ostream& err);

}  // namespace mcc::stages

#endif  // MCC_STAGES_CHUNK_EMIT_H
