// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — forge_core: engine-free TERRAIN-SEAM stub (issue #134).
//
// The one *meaningful* forge_core call this M0 skeleton exposes is a stub of the
// `ITerrainBackend` region-alignment query (Tools SAD §5.2, operation 3):
//
//   "Region-alignment query — report the backend's region size and assert it
//    tiles on the 128 m chunk grid (§3.2); reject a backend/config that cannot
//    align. For Terrain3D this pins region_size = SIZE_128 so a region == one
//    chunk exactly."
//
// This is deliberately the *pure-logic* half of that op — no Terrain3D, no Godot.
// It is the single place the "does a terrain region tile the 128 m Meridian chunk
// grid?" rule is defined, so the real Terrain3DBackend and a future in-house
// MeridianTerrainBackend (SAD §5.2, the A-09 swap seam) can share one predicate
// instead of each open-coding an alignment check. Unit-tested as pure logic
// (forge_terrain_stub_test.cpp) with NO Godot runtime.

#ifndef FORGE_CORE_FORGE_TERRAIN_STUB_H
#define FORGE_CORE_FORGE_TERRAIN_STUB_H

namespace forge::terrain {

// The Meridian chunk grid pitch in metres (Tools SAD §3.2 / §5.2). Zones tile on
// this 128 m grid; a terrain backend's region size must align to it.
inline constexpr double kChunkSizeM = 128.0;

// The per-chunk server heightfield side (Tools SAD §5.2 op 4): a 129×129
// row-major float32 grid at 1 m spacing (128 m span + the shared edge sample).
inline constexpr int kHeightfieldSide = 129;

// The region size the chosen Terrain3D backend pins (A-09 → Sync Decisions §11):
// Terrain3D `region_size = SIZE_128`, i.e. one region == one 128 m chunk exactly.
inline constexpr double kTerrain3DRegionSizeM = 128.0;

// Does a terrain region of `region_m` metres tile the `chunk_m` chunk grid?
//
// True iff both sizes are positive AND the larger is an exact integer multiple of
// the smaller — i.e. either several whole regions fit a chunk, or a region spans
// a whole number of chunks. This is the alignment rule op 3 enforces; a backend
// whose region size fails it must be rejected. Pure; never throws.
bool region_tiles_chunk_grid(double region_m, double chunk_m = kChunkSizeM);

}  // namespace forge::terrain

#endif  // FORGE_CORE_FORGE_TERRAIN_STUB_H
