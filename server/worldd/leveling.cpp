// SPDX-License-Identifier: Apache-2.0
//
// worldd — XP / leveling curve implementation (issue #360, CHR-03). See leveling.h
// for the clean-room provenance and the M1-placeholder / world-DB-curve seam.

#include "leveling.h"

namespace meridian::worldd {

std::uint32_t xp_to_next_level(std::uint16_t level) {
    if (level == 0 || level >= kMaxLevel) return 0;  // capped: no further progression
    const std::uint64_t l = level;
    const std::uint64_t v = kXpLinear * l + kXpQuadratic * l * l;
    return static_cast<std::uint32_t>(v);
}

std::uint32_t xp_for_kill(std::uint16_t victim_level, std::uint16_t killer_level) {
    const std::uint16_t vl = victim_level > 0 ? victim_level : 1;
    const std::uint32_t base = kXpKillBase +
                               kXpKillPerVictimLevel * static_cast<std::uint32_t>(vl);

    // Grey-mob falloff: only when the killer OUTLEVELS the victim.
    if (killer_level > vl) {
        const std::uint16_t gap = static_cast<std::uint16_t>(killer_level - vl);
        if (gap >= kGreyLevelGap) return 0;  // trivial kill — no XP
        // Linear falloff across the gap: full at gap 0, →0 as gap → kGreyLevelGap.
        const std::uint32_t num = static_cast<std::uint32_t>(kGreyLevelGap - gap);
        const std::uint32_t scaled = (base * num) / kGreyLevelGap;
        return scaled > 0 ? scaled : 1;  // a non-grey kill always awards >= 1
    }
    return base;  // same-or-higher-level victim: full base
}

LevelProgress grant_xp(std::uint16_t level, std::uint32_t xp_into_level, std::uint32_t xp) {
    LevelProgress p;
    p.level = level > 0 ? level : 1;
    p.xp_into_level = xp_into_level;
    p.levels_gained = 0;

    // Accumulate, then roll over each crossed threshold. At kMaxLevel the threshold
    // is 0 (xp_to_next_level returns 0), so the loop stops and progress is pinned.
    std::uint64_t acc = static_cast<std::uint64_t>(p.xp_into_level) + xp;
    while (p.level < kMaxLevel) {
        const std::uint32_t need = xp_to_next_level(p.level);
        if (need == 0 || acc < need) break;
        acc -= need;
        ++p.level;
        ++p.levels_gained;
    }
    // At the cap, drop any residual (no partial progress past max).
    p.xp_into_level = (p.level >= kMaxLevel) ? 0 : static_cast<std::uint32_t>(acc);
    return p;
}

}  // namespace meridian::worldd
