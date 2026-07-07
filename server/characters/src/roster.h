// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters — the M0-frozen playable roster (CHR-01 stub, D-11).
//
// SINGLE CANONICAL SOURCE of the M0 race/class enumeration. The character
// stub CRUD (create) validates the requested race/class against THIS list; the
// characters DB stores them as the `character.race` / `character.class` TINYINT
// UNSIGNED ids (server/db/characters/migrations/0001_init_characters.up.sql).
//
// PROVENANCE / why these ids exist here and not in the DB:
//   * Sync decision D-11 (docs/01-SYNC-DECISIONS.md) fixes the M0 CHR-01 stub
//     scope as "name entry + class selection over one placeholder model" — no
//     appearance customization. The stub still writes a race + class id per row
//     (the schema columns are NOT NULL), so the server needs a fixed, validated
//     id set from M0.
//   * The Game Design Baseline (docs/00-GAME-DESIGN-BASELINE.md §3) fixes only
//     the COUNTS per milestone — M1: "1 playable race, 2 classes (1 melee, 1
//     caster)"; M2: "2 races, 4 classes" — and does NOT enumerate names/ids.
//     There is therefore no authoritative name list to copy; these ids/names
//     are an ORIGINAL, clean-room placeholder set (CONTRIBUTING.md — no
//     Blizzard/WoW roster, no GPL source consulted), deliberately generic
//     archetypes, defined here so validation has something concrete to reject.
//
// M0-FROZEN: ids are STABLE (a persisted character.race/class is these ids);
// names may be re-skinned by design later, but an id NEVER changes meaning and
// ids are append-only (the real M1/M2 roster extends this list, it never
// renumbers it). Ids are 1-based; 0 is reserved as "unset/invalid" so a
// zero-initialised field never accidentally validates.

#pragma once

#include <cstdint>
#include <string_view>

namespace meridian::characters {

// M0-frozen playable races (original placeholder roster — see file header).
// id 1 (Ardent) is the M1 "1 playable race"; 2..4 are frozen placeholders that
// become playable as later milestones turn them on. Ids are append-only.
enum class Race : std::uint8_t {
    kArdent  = 1,  // resilient folk of the central realm (M1 playable race)
    kDolmen  = 2,  // mountain/stone folk
    kSylvane = 3,  // forest folk
    kEmberkin = 4, // fire-touched folk
};

// M0-frozen playable classes (original placeholder roster — see file header).
// M1 requires 1 melee + 1 caster: kVanguard (melee) + kRuncaller (caster).
enum class Class : std::uint8_t {
    kVanguard  = 1,  // front-line melee (M1 melee class)
    kRuncaller = 2,  // arcane caster    (M1 caster class)
    kWarden    = 3,  // ranged/hybrid
    kMender    = 4,  // healer
};

// Count of ids in each M0-frozen enum. Valid ids are the contiguous range
// [1, kRaceCount] / [1, kClassCount]. Bump these (and add the enumerator) when
// a milestone turns on more of the roster — never renumber existing ids.
inline constexpr std::uint8_t kRaceCount  = 4;
inline constexpr std::uint8_t kClassCount = 4;

// True iff `race` is a defined M0-frozen race id (rejects 0 and out-of-range).
constexpr bool is_valid_race(std::uint8_t race) {
    return race >= 1 && race <= kRaceCount;
}

// True iff `cls` is a defined M0-frozen class id (rejects 0 and out-of-range).
constexpr bool is_valid_class(std::uint8_t cls) {
    return cls >= 1 && cls <= kClassCount;
}

// Human-readable name for a race/class id (diagnostics/logging only — the DB
// stores the numeric id). Returns "" for an unknown id.
constexpr std::string_view race_name(std::uint8_t race) {
    switch (race) {
        case 1: return "Ardent";
        case 2: return "Dolmen";
        case 3: return "Sylvane";
        case 4: return "Emberkin";
        default: return "";
    }
}

constexpr std::string_view class_name(std::uint8_t cls) {
    switch (cls) {
        case 1: return "Vanguard";
        case 2: return "Runcaller";
        case 3: return "Warden";
        case 4: return "Mender";
        default: return "";
    }
}

}  // namespace meridian::characters
