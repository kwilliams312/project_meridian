// SPDX-License-Identifier: Apache-2.0
//
// worldd — XP curve / leveling UNIT test (issue #360, CHR-03). See leveling.h.
//
// PURE (no DB, no socket, no clock, no RNG): drives the leveling curve directly —
// xp_to_next_level monotonicity + cap, xp_for_kill grey-mob falloff, grant_xp
// roll-over (single, multi-level, and the level cap), and a full 1→5 grind proof.
// Deterministic; always runs in the plain server ctest.

#include "leveling.h"

#include <cstdio>

using namespace meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("worldd leveling curve (#360)\n");

    // --- xp_to_next_level: the M1 placeholder curve values + monotonicity + cap. --
    check("L1->2 threshold = 50", xp_to_next_level(1) == 50);
    check("L2->3 threshold = 120", xp_to_next_level(2) == 120);
    check("L3->4 threshold = 210", xp_to_next_level(3) == 210);
    check("L4->5 threshold = 320", xp_to_next_level(4) == 320);
    bool monotonic = true;
    for (std::uint16_t l = 1; l + 1 < kMaxLevel; ++l)
        if (!(xp_to_next_level(l) < xp_to_next_level(l + 1))) monotonic = false;
    check("threshold strictly increases with level", monotonic);
    check("threshold at cap is 0 (no progression)", xp_to_next_level(kMaxLevel) == 0);
    check("threshold at level 0 is 0", xp_to_next_level(0) == 0);

    // --- xp_for_kill: full base at/above level, grey falloff below. --------------
    check("L1 kills L1: full base 25", xp_for_kill(/*victim=*/1, /*killer=*/1) == 25);
    check("higher-level victim: still full base",
          xp_for_kill(/*victim=*/5, /*killer=*/1) == kXpKillBase + 5 * kXpKillPerVictimLevel);
    check("grey mob (gap >= kGreyLevelGap) awards 0",
          xp_for_kill(/*victim=*/1, /*killer=*/1 + kGreyLevelGap) == 0);
    // Falloff is monotonically non-increasing as the killer outlevels a fixed victim.
    bool falloff_ok = true;
    std::uint32_t prev = xp_for_kill(1, 1);
    for (std::uint16_t k = 1; k <= 1 + kGreyLevelGap; ++k) {
        const std::uint32_t x = xp_for_kill(1, k);
        if (x > prev) falloff_ok = false;
        prev = x;
    }
    check("kill XP falls off as killer outlevels the victim", falloff_ok);
    check("a non-grey kill always awards >= 1", xp_for_kill(1, 5) >= 1);

    // --- grant_xp: no level-up, single, multi-level, and the cap. ----------------
    {
        const LevelProgress p = grant_xp(/*level=*/1, /*into=*/0, /*xp=*/49);
        check("below threshold: no level-up", p.level == 1 && p.levels_gained == 0 &&
                                                  p.xp_into_level == 49);
    }
    {
        const LevelProgress p = grant_xp(/*level=*/1, /*into=*/0, /*xp=*/50);
        check("exactly threshold: one level-up, 0 leftover",
              p.level == 2 && p.levels_gained == 1 && p.xp_into_level == 0);
    }
    {
        const LevelProgress p = grant_xp(/*level=*/1, /*into=*/10, /*xp=*/55);
        check("carry-over: into=10 + 55 = 65 → L2 with 15 leftover",
              p.level == 2 && p.levels_gained == 1 && p.xp_into_level == 15);
    }
    {
        // 50 + 120 + 210 + 320 = 700 crosses L1→L5 exactly.
        const LevelProgress p = grant_xp(/*level=*/1, /*into=*/0, /*xp=*/700);
        check("one big award grants multiple levels (1→5)",
              p.level == 5 && p.levels_gained == 4 && p.xp_into_level == 0);
    }
    {
        const LevelProgress p = grant_xp(/*level=*/kMaxLevel, /*into=*/0, /*xp=*/1000000);
        check("at the cap, XP is discarded (no overflow past max)",
              p.level == kMaxLevel && p.levels_gained == 0 && p.xp_into_level == 0);
    }

    // --- End-to-end grind: killing L1 creatures reaches L5 in finite kills. -------
    {
        std::uint16_t level = 1;
        std::uint32_t into = 0;
        int kills = 0;
        while (level < 5 && kills < 10000) {
            const std::uint32_t xp = xp_for_kill(/*victim=*/1, level);
            const LevelProgress p = grant_xp(level, into, xp);
            level = p.level;
            into = p.xp_into_level;
            ++kills;
        }
        std::printf("  ..    (grind: reached level %u in %d kills)\n", level, kills);
        check("grinding L1 creatures reaches L5", level == 5);
    }

    std::printf("\n%s (%d failures)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
