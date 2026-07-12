// Project Meridian — M1 heightfield ground-sample backend for the kinematic
// movement controller (issue #557, Epic #22 Story D). This is the real
// `IWorldQuery` implementation that replaces the M0 flat stub (FlatWorldQuery,
// movement_query.h) behind the SAME seam: the controller (#102) swaps backends
// with NO caller change.
//
// Q1(a) — the client ships the server `.chunk.bin` per chunk, so it reads the
// SAME 129×129 f32 `Heightfield` table (schema/chunk/chunk.fbs §3.2) worldd
// validates movement against, and samples it with the SAME routine. Sampling the
// identical bytes with the identical formula makes client prediction and server
// validation agree BY CONSTRUCTION (movement_constants.h §7; Server SAD §5.5,
// z within ±4 m — kHeightTolerance).
//
// ENGINE-FREE (Client SAD §9.2): plain C++17, NO Godot types and NO FlatBuffers
// dependency — it operates on an already-decoded heightfield grid. The `.chunk.bin`
// bytes are turned into a HeightfieldChunk by heightfield_chunk_decode.* (which
// DOES use the flatc bindings); keeping the sampler + query dependency-free lets
// them link into the plain doctest suite exactly like movement_controller.*.

#ifndef MERIDIAN_HEIGHTFIELD_QUERY_H
#define MERIDIAN_HEIGHTFIELD_QUERY_H

#include "movement_constants.h"
#include "movement_query.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace meridian::movement {

// One resident chunk's heightfield grid — the `Heightfield` table decoded out of
// a `<cx>_<cz>.chunk.bin` (schema/chunk/chunk.fbs §3.2): `side`×`side` row-major
// (z-outer, x-inner) f32 samples at `spacing_m` metre spacing, in zone-local
// metres. `cx,cz` is the AoI grid cell (== chunk.fbs ChunkCoord). The shared-edge
// convention (row/col `side-1` duplicates the neighbour's row/col 0) means a seam
// samples identically from either chunk.
struct HeightfieldChunk {
	int cx = 0;
	int cz = 0;
	std::uint16_t side = static_cast<std::uint16_t>(kHeightfieldSide);  // 129
	float spacing_m = kHeightfieldSpacingM;                            // 1.0
	std::vector<float> samples;   // side*side, row-major index = z*side + x
};

// The outcome of a bilinear ground sample: the interpolated height and whether
// the cell is a hole. `hole` is true iff any of the four lattice corners is NaN
// (an un-sculpted / hole cell carries the exporter's NaN sentinel —
// schema/chunk-payload.md); the mover is NOT walkable over a hole.
struct HeightfieldSampleResult {
	float height = 0.0f;
	bool  hole   = false;
};

// ── THE SHARED, MIRRORABLE SAMPLING ROUTINE ──────────────────────────────────
// Bilinear-interpolate a `side`×`side` row-major (z-outer, x-inner) heightfield
// at a LOCAL metre offset (local_x, local_z) inside the chunk, each in the closed
// range [0, (side-1)*spacing_m]. This is the ONE formula the client kinematic
// controller and the server movement validator MUST agree on (movement_constants.h
// §7; Server SAD §5.5), so it lives in a single engine-free routine both build
// trees include / mirror — the same-formula half of the Q1(a) same-bytes guarantee.
//
// EXACT FORMULA (documented so the server track can share or reproduce it):
//     gx = local_x / spacing_m,            gz = local_z / spacing_m
//     ix = clamp(floor(gx), 0, side-2),    iz = clamp(floor(gz), 0, side-2)
//     fx = gx - ix,                        fz = gz - iz
//     h00 = S[iz*side + ix]        h10 = S[iz*side + ix + 1]
//     h01 = S[(iz+1)*side + ix]    h11 = S[(iz+1)*side + ix + 1]
//     h0  = h00 + (h10 - h00)*fx   h1  = h01 + (h11 - h01)*fx
//     height = h0 + (h1 - h0)*fz
// At an integer lattice offset the result is EXACTLY the stored sample (fx or fz
// == 0). If any of the four corners is NaN the result carries hole=true and a NaN
// height (the caller reports the cell as not walkable). A null/degenerate grid
// (side < 2, spacing <= 0) is treated as a hole rather than dereferenced.
HeightfieldSampleResult sample_heightfield_bilinear(const float* samples,
                                                    std::uint16_t side,
                                                    float spacing_m,
                                                    float local_x,
                                                    float local_z);

// M1 `IWorldQuery` backend (issue #557): resolves ground height by bilinear-
// sampling the per-chunk heightfield the pack ships (IF-6), replacing the M0
// FlatWorldQuery constant plane behind the SAME `IWorldQuery` seam so the
// movement controller (#102) swaps backends with no caller change.
//
// It owns the zone grid geometry (origin + chunk size, matching the IF-6 manifest
// `origin` / `chunk_size_m`) and a set of resident chunks keyed by (cx,cz).
// `sample_ground(x, z)`:
//   • maps world XZ -> the containing chunk cell + the local offset within it,
//   • bilinear-samples that chunk's grid via the shared routine above,
//   • returns walkable=false over a NaN hole OR when no chunk covers XZ (out of
//     bounds) OR when a resident chunk is malformed — never a silent flat guess
//     (Client SAD §5.2 "never a silent degrade").
class HeightfieldWorldQuery final : public IWorldQuery {
public:
	// `origin_x/z` and `chunk_size_m` come from the IF-6 zone manifest
	// (zone01.chunks.json `origin` / `chunk_size_m`).
	HeightfieldWorldQuery(float origin_x, float origin_z,
	                      float chunk_size_m = kChunkSizeM);

	// Make a chunk resident (replaces any existing chunk at the same coord).
	void add_chunk(HeightfieldChunk chunk);

	// Number of resident chunks.
	std::size_t chunk_count() const { return chunks_.size(); }

	// IWorldQuery: ground under (x, z) in zone-local metres.
	GroundSample sample_ground(float x, float z) const override;

private:
	const HeightfieldChunk* find_chunk(int cx, int cz) const;

	float origin_x_;
	float origin_z_;
	float chunk_size_m_;
	std::vector<HeightfieldChunk> chunks_;
};

}  // namespace meridian::movement

#endif  // MERIDIAN_HEIGHTFIELD_QUERY_H
