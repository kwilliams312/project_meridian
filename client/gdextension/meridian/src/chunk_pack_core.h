// Project Meridian — engine-free client CHUNK-PACK verify core (issue #554,
// Epic #22 Story A). The fail-closed packaging layer that sits ON TOP of the
// #107 IF-5 pack-manifest verify (pack_manifest_core.*): once the pack manifest
// is trusted, THIS core loads the IF-6 zone chunk index (`<zone>.chunks.json`),
// resolves every per-chunk ref through the IF-8 asset-id table
// (`<zone>.assets.json`), and proves the pack is COMPLETE and INTACT before the
// world-enter path is allowed to proceed (Tools SAD §3.3; the A-08 walk
// amendments C2/C5; Client SAD §2.3 "mount … verify … then enter").
//
// WHY (the fail-closed contract, Client SAD §5.2 "never a silent degrade"): a
// content pack the client would stream a zone from can be INCOMPLETE (a partial
// download / a bad mirror dropped a chunk payload), TAMPERED (a byte flipped on
// disk or in transit), or DRIFTED (a manifest ref that resolves to nothing).
// Spawning into a half-loaded or corrupt map is worse than refusing — so this
// core refuses. Every referenced asset MUST resolve to a real res:// path, MUST
// exist in the mounted pack, and every chunk's declared BLAKE3 MUST match the
// bytes actually on disk. Any failure is a HARD failure with a clear reason the
// boot / world-enter flow surfaces and blocks on.
//
// ENGINE-FREE (Client SAD §9.2): plain C++17, NO Godot types, exactly like the
// #107 pack-manifest core / the movement + telemetry cores. Disk access is
// abstracted behind IPackFileProvider so the SAME verify logic runs (a) under
// the GDExtension over Godot FileAccess (res:// / a mounted `.pck`), and (b) in
// a plain unit test over std::filesystem — no Godot runtime needed to test it.
//
// HASHING (issue directive: reuse, do NOT add a second BLAKE3): the per-chunk
// integrity hash is recomputed with the SAME vendored BLAKE3 the mcc emitter
// uses (tools/mcc/src/hash/blake3.*), framed BYTE-IDENTICALLY to mcc's
// chunk_emit `entry_hash_of` — "server\0<bytes>\0scene\0<bytes>\0proxy\0<bytes>\0",
// rendered "blake3:<64hex>" — so the recomputed hash and the manifest hash agree
// exactly (proven by the round-trip test against Story-0's fixture).

#ifndef MERIDIAN_CHUNK_PACK_CORE_H
#define MERIDIAN_CHUNK_PACK_CORE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace meridian::chunkpack {

// The IF-6 manifest format-major THIS client can read. `<zone>.chunks.json`
// stamps `"format_version": 1`; loaders reject unknown majors (Tools SAD §3.4).
inline constexpr int kSupportedManifestFormatVersion = 1;

// The IF-8 asset-map schema tag THIS client understands (mcc chunk_emit stamps
// `"schema": "meridian/chunk-assetmap@1"`). Any other tag is a map format this
// client cannot read -> malformed.
inline constexpr const char* kSupportedAssetMapSchema = "meridian/chunk-assetmap@1";

// A BLAKE3-256 rendered lowercase-hex is exactly 64 chars (mirrors the #107
// core's kContentHashHexLen). The manifest per-chunk hash carries a "blake3:"
// prefix (the IF-6 contentHash grammar); the asset-map per-resource hash is bare
// 64-hex.
inline constexpr std::size_t kBlake3HexLen = 64;

// ── Value shapes (zone-local metres; Tools SAD §3.1) ─────────────────────────
struct Vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct Aabb {
	Vec3 min;
	Vec3 max;
};

// A logical ref (asset id, e.g. "core:chunk.zone01.0_0.server") resolved through
// the IF-8 table to its concrete res:// path (A-08 amendment C2: manifest refs
// are ID-derived, never raw paths — the path is derived at load).
struct ResolvedRef {
	std::string ref;        // the logical id from the manifest
	std::string res_path;   // the resolved res:// path (from the asset table)
};

// One resolved chunk from the manifest `chunks[]` (Tools SAD §3.3): its grid
// coord (may be negative — §3.1), its declared both-payloads hash, and every ref
// resolved to a res:// path. `has_proxy` is false for the explicit `proxy: null`
// form (amendment C3).
struct ResolvedChunk {
	int cx = 0;
	int cz = 0;
	std::string hash;       // "blake3:<64hex>" — over BOTH payloads (§3.3)
	ResolvedRef scene;
	bool has_proxy = false;
	ResolvedRef proxy;      // valid only when has_proxy
	ResolvedRef server;
	std::vector<ResolvedRef> deps;   // shared IF-8 prefetch refs (may be payload-less at v0)
	bool has_priority = false;
	int priority = 0;
	Aabb aabb;
};

// One IF-8 asset-map entry (`<zone>.assets.json` entries[]): the logical id ->
// concrete res:// path + per-resource bare 64-hex BLAKE3.
struct AssetEntry {
	std::string id;
	std::uint32_t numeric_id = 0;
	std::string type;
	std::string res_path;
	std::string hash;       // bare 64-hex (per-resource)
};

// The parsed + resolved zone chunk index — the in-memory model the streamer
// would drive (coord, aabb, priority, resolved paths, deps).
struct ChunkIndex {
	int format_version = 0;
	std::string zone;
	int chunk_size_m = 0;
	Vec3 origin;            // only x,z meaningful (XZ plane)
	int min_cx = 0, min_cz = 0, max_cx = 0, max_cz = 0;
	int far_ring = 0;
	std::vector<ResolvedChunk> chunks;   // in manifest (file) order
};

// How the fail-closed verify came out. kOk is the ONLY verdict that lets the
// world-enter path proceed; every other is a HARD failure (hard_fail == true).
enum class ChunkPackVerdict {
	kOk,                  // parsed, every ref resolved, every payload present + intact
	kManifestMalformed,   // chunks.json / assets.json won't parse, wrong schema tag,
	                      // unknown format major, or a missing/blank required field
	kUnresolvedRef,       // a scene/proxy/server/dep ref is not in the IF-8 asset table
	kMissingAsset,        // a referenced payload is absent from the mounted pack
	kHashMismatch,        // a chunk's recomputed BLAKE3 != its manifest hash (tamper/corrupt)
};

// The verify outcome (mirrors the #107 PackReport shape): the verdict, a
// human-readable reason for the boot/world-enter log, the resolved index (filled
// as far as parsing got), and the completeness counters.
struct ChunkPackReport {
	ChunkPackVerdict verdict = ChunkPackVerdict::kManifestMalformed;
	bool ok = false;         // verdict == kOk
	bool hard_fail = true;   // true unless verdict == kOk
	std::string reason;      // one-line explanation for logs / boot UX

	ChunkIndex index;        // resolved chunk index (partial on an early failure)
	std::size_t chunk_count = 0;      // chunks declared in the manifest
	std::size_t verified_chunks = 0;  // chunks that passed presence + integrity
};

// Disk seam: the engine-free core reads pack bytes through this. The GDExtension
// wrapper implements it over Godot FileAccess (res:// aware, a mounted `.pck`);
// tests implement it over std::filesystem. `exists` is the presence check;
// `read` returns the full byte payload for the integrity re-hash.
struct IPackFileProvider {
	virtual ~IPackFileProvider() = default;
	// True iff a payload exists at this res:// path in the mounted pack.
	virtual bool exists(const std::string& res_path) const = 0;
	// Read the full bytes at res_path into out. Returns false if unreadable
	// (missing / open error); out is only meaningful when it returns true.
	virtual bool read(const std::string& res_path, std::string& out) const = 0;
};

// ── Stage 1: parse + resolve (NO disk) ───────────────────────────────────────
// Parse `<zone>.chunks.json` (IF-6) + `<zone>.assets.json` (IF-8) and resolve
// every per-chunk ref (scene / proxy / server / deps) through the asset table.
// Fail-fast:
//   (1) both JSONs parse + carry the expected format major / schema tag  -> else kManifestMalformed
//   (2) required manifest/asset fields present + well-formed             -> else kManifestMalformed
//   (3) every chunk ref resolves to an asset-table res:// path           -> else kUnresolvedRef
// The resolved index (report.index) is filled through the last stage reached, so
// the caller can inspect what parsed even on a resolution failure. This stage
// does NOT touch the filesystem — it is the pure, unit-testable half.
ChunkPackReport build_and_resolve(const std::string& chunks_json,
                                  const std::string& assets_json);

// ── Stage 2: full fail-closed verify (presence + integrity, needs the pack) ──
// build_and_resolve(...) THEN, per chunk in manifest order:
//   (4) presence — scene / proxy / server payloads exist in the pack  -> else kMissingAsset
//   (5) integrity — recompute the both-payloads BLAKE3 from the bytes on disk
//       and match the chunk's manifest hash                            -> else kHashMismatch
// The FIRST failure returns immediately with a clear reason (fail closed — the
// world-enter path must block). `deps` are prefetch-only logical refs (a shared
// asset may ship outside this chunk pack), so they are REQUIRED TO RESOLVE
// (stage 3) but NOT required to be present here — a payload-less dep is not a
// completeness failure at v0 (Tools SAD §3.3 "prefetch"; the Story-0 fixture's
// shared ground dep has no shipped payload). report.verified_chunks counts the
// chunks that passed both (4) and (5); on kOk it equals chunk_count.
ChunkPackReport verify_chunk_pack(const std::string& chunks_json,
                                  const std::string& assets_json,
                                  const IPackFileProvider& files);

// The EXACT per-chunk integrity hash construction — BLAKE3 over the two payloads
// framed "server\0<bytes>\0scene\0<bytes>\0proxy\0<bytes>\0", rendered
// "blake3:<64hex>". Byte-identical to mcc chunk_emit `entry_hash_of`; a chunk
// with `proxy: null` frames an EMPTY proxy payload. Exposed for the verify path
// and the round-trip test.
std::string recompute_chunk_hash(const std::string& server_bytes,
                                 const std::string& scene_bytes,
                                 const std::string& proxy_bytes);

// Human-readable verdict name (logs / test diagnostics).
const char* chunk_pack_verdict_name(ChunkPackVerdict v);

}  // namespace meridian::chunkpack

#endif  // MERIDIAN_CHUNK_PACK_CORE_H
