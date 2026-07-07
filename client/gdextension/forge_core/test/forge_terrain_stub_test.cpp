// SPDX-License-Identifier: Apache-2.0
//
// Engine-free unit test for the forge_core terrain-seam stub (issue #134).
// Proves the ITerrainBackend region-alignment rule (Tools SAD §5.2 op 3) that the
// real Terrain3DBackend and a future in-house backend both rely on. No Godot.

#include "forge_terrain_stub.h"

#include <cstdio>

namespace ft = forge::terrain;

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

int main() {
    // The Terrain3D pin (A-09): region == one 128 m chunk exactly -> aligns.
    check(ft::region_tiles_chunk_grid(ft::kTerrain3DRegionSizeM), "Terrain3D SIZE_128 tiles the grid");
    check(ft::region_tiles_chunk_grid(128.0, 128.0), "128 m region == 128 m chunk");

    // Several whole regions per chunk (region divides the chunk).
    check(ft::region_tiles_chunk_grid(64.0), "64 m region tiles a 128 m chunk (2/chunk)");
    check(ft::region_tiles_chunk_grid(32.0), "32 m region tiles a 128 m chunk (4/chunk)");
    check(ft::region_tiles_chunk_grid(1.0), "1 m region tiles a 128 m chunk");

    // A region spanning a whole number of chunks (chunk divides the region).
    check(ft::region_tiles_chunk_grid(256.0), "256 m region spans 2 chunks");
    check(ft::region_tiles_chunk_grid(1024.0), "1024 m region spans 8 chunks");

    // Non-aligning sizes must be rejected.
    check(!ft::region_tiles_chunk_grid(100.0), "100 m region does NOT tile a 128 m chunk");
    check(!ft::region_tiles_chunk_grid(48.0), "48 m region does NOT tile a 128 m chunk");
    check(!ft::region_tiles_chunk_grid(130.0), "130 m region does NOT tile a 128 m chunk");

    // Degenerate inputs are safe (reject, never UB).
    check(!ft::region_tiles_chunk_grid(0.0), "zero region rejected");
    check(!ft::region_tiles_chunk_grid(-128.0), "negative region rejected");
    check(!ft::region_tiles_chunk_grid(128.0, 0.0), "zero chunk rejected");

    // A custom (non-default) chunk pitch still enforces the multiple rule.
    check(ft::region_tiles_chunk_grid(16.0, 64.0), "16 m region tiles a 64 m grid");
    check(!ft::region_tiles_chunk_grid(24.0, 64.0), "24 m region does NOT tile a 64 m grid");

    // The heightfield side is the SAD-fixed 129 (128 m span + shared edge).
    check(ft::kHeightfieldSide == 129, "heightfield side is 129");

    if (g_failures == 0) {
        std::printf("forge_terrain_stub: all checks passed\n");
        return 0;
    }
    std::printf("forge_terrain_stub: %d check(s) FAILED\n", g_failures);
    return 1;
}
