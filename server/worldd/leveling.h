// SPDX-License-Identifier: Apache-2.0
//
// worldd — XP / leveling curve (issue #360, CHR-03; part of epic #19). The pure
// progression math the "unit died" hook consumes when a player kills a creature:
//
//   • xp_for_kill()      — how much XP a kill awards, with a level-difference
//                          falloff (a much-lower "grey" victim gives nothing).
//   • xp_to_next_level()  — the XP threshold to advance one level (the curve).
//   • grant_xp()          — accumulate XP and roll over any level-ups.
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md (CHR-03: "XP awards (kills,
// quests), level-up stat application from class/level tables (world DB)") and
// docs/sad/server-sad.md §2.5 ONLY. Every constant and formula here is ORIGINAL —
// a simple monotonic quadratic curve derived from first principles, NOT lifted
// from any existing game's XP tables. No GPL / AGPL / CMaNGOS / TrinityCore /
// leaked emulator source consulted. See CONTRIBUTING.md.
//
// M1 PLACEHOLDER: the numbers below are the M1 stand-in. The real per-level curve
// and per-class/level stat tables load from the world DB (IF-4) via content epic
// #28; this module is the documented SEAM until then — swap the formula bodies for
// a world-DB-loaded table without changing callers.
//
// PURE / DB-FREE / SOCKET-FREE: plain integer math over the #342 Unit's level.
// No socket, DB, FlatBuffer, RNG, or clock — fully unit-testable in the plain
// `server` ctest, and deterministic (leveling must be server-authoritative and
// reproducible).

#ifndef MERIDIAN_WORLDD_LEVELING_H
#define MERIDIAN_WORLDD_LEVELING_H

#include <cstdint>

namespace meridian::worldd {

// The M1 placeholder level cap. The curve is defined for levels [1, kMaxLevel];
// a player at kMaxLevel earns no further XP (the threshold is "infinite").
inline constexpr std::uint16_t kMaxLevel = 60;

// XP-curve constants (ORIGINAL clean-room — see header). The threshold to advance
// FROM level L is a monotonic quadratic: base grind + a quadratic ramp so later
// levels take longer. xp_to_next_level(L) = kXpLinear*L + kXpQuadratic*L*L.
inline constexpr std::uint32_t kXpLinear = 40;
inline constexpr std::uint32_t kXpQuadratic = 10;

// Kill-XP constants. A kill's base award scales with the VICTIM level; a killer
// far above the victim ("grey" mob) earns nothing. Linear falloff across the gap.
inline constexpr std::uint32_t kXpKillBase = 20;        // flat floor per kill
inline constexpr std::uint32_t kXpKillPerVictimLevel = 5;  // + per victim level
inline constexpr std::uint16_t kGreyLevelGap = 6;       // victim this far below → 0 XP

// XP required to advance FROM `level` to `level + 1`. Returns 0 at/above kMaxLevel
// (capped — no further progression). Monotonic increasing over [1, kMaxLevel).
std::uint32_t xp_to_next_level(std::uint16_t level);

// XP a killer of `killer_level` earns for killing a victim of `victim_level`.
// A victim kGreyLevelGap or more levels BELOW the killer is "grey" and awards 0
// (attribution via the threat table is the caller's job — this is just the curve).
// A same-or-higher-level victim awards the full base. Never overflows.
std::uint32_t xp_for_kill(std::uint16_t victim_level, std::uint16_t killer_level);

// The result of granting XP: the level after any roll-ups, the leftover XP into
// that level, and how many levels were gained (0 = no level-up).
struct LevelProgress {
    std::uint16_t level = 1;          // level after applying the award
    std::uint32_t xp_into_level = 0;  // XP accumulated toward the NEXT level
    std::uint16_t levels_gained = 0;  // number of level-ups this award triggered
};

// Add `xp` to a player currently at `level` with `xp_into_level` progress, rolling
// over as many level thresholds as the award crosses (a big award can grant
// multiple levels). At kMaxLevel the player stops accumulating (xp_into_level 0).
// Deterministic and total (no I/O).
LevelProgress grant_xp(std::uint16_t level, std::uint32_t xp_into_level, std::uint32_t xp);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_LEVELING_H
