// SPDX-License-Identifier: Apache-2.0
//
// forge_core TERRAIN-SEAM stub implementation (issue #134). See
// forge_terrain_stub.h for the rule + rationale (Tools SAD §5.2 op 3).

#include "forge_terrain_stub.h"

#include <cmath>

namespace forge::terrain {

bool region_tiles_chunk_grid(double region_m, double chunk_m) {
    if (!(region_m > 0.0) || !(chunk_m > 0.0)) {
        return false;  // non-positive (incl. NaN, which fails every comparison)
    }

    // The larger size must be an exact integer multiple of the smaller: either
    // N whole regions fit one chunk, or a region spans N whole chunks.
    const double big = region_m > chunk_m ? region_m : chunk_m;
    const double small = region_m > chunk_m ? chunk_m : region_m;
    const double ratio = big / small;
    const double rounded = std::round(ratio);

    // Relative tolerance so clean multiples (e.g. 256/128, 128/32) pass despite
    // binary float rounding, without admitting near-misses.
    return rounded >= 1.0 && std::fabs(ratio - rounded) <= 1e-9 * ratio;
}

}  // namespace forge::terrain
