// SPDX-License-Identifier: Apache-2.0
//
// meridian-characters — the runtime playable roster (CHR-01 / SP2.5 #695).
//
// SOURCE OF TRUTH = PACK DATA. The valid race/class set is loaded from the
// compiled content pack (the `race` / `class` world-DB tables mcc emit-sql fills
// from schema/content/{race,class}.schema.yaml), NOT a hardcoded enum. This
// retires the old compiled Race/Class enum + is_valid_race/is_valid_class free
// functions (SP2 design §2.2): worldd loads the pack roster at boot into a runtime
// `Roster`, and character CREATE validates the requested race/class against it.
//
// TRANSITIONAL COMPILED FALLBACK (shrinking; full deletion deferred to SP5).
// The M0 roster is four races (Ardent/Dolmen/Sylvane/Emberkin) and four classes
// (Vanguard/Runcaller/Warden/Mender), but the seed pack can only author the ones
// that have the content they REQUIRE today: Ardent/Dolmen (they have appearance
// catalogs) and Vanguard/Warden. Sylvane/Emberkin need appearance content and
// Runcaller/Mender need abilities/talent trees that only arrive with the SP5
// chibi content — until then they CANNOT live in the pack. So this file keeps a
// thin COMPILED FALLBACK for exactly those four not-yet-in-pack entries, which the
// runtime `Roster` MERGES on top of the pack entries at load. As SP5 authors the
// remaining entries in-pack, this fallback shrinks to nothing and the file is
// deleted outright (the true end state of "roster from pack data").
//
// NUMERIC-ID CONTRACT (append-only; NEVER renumber). A roster id is the STABLE
// 1-based small id persisted in `character.race` / `character.class`
// (server/db/characters/migrations/0001_init_characters.up.sql — TINYINT UNSIGNED)
// and sent by the client roster mirror (client .../charselect/character_roster.gd).
// It is NOT the IF-9 content id. The pack carries it as the `roster_id` field so a
// persisted id keeps its meaning across the roster.h retirement. 0 is reserved as
// "unset/invalid" so a zero-initialised field never accidentally validates.
//
// Clean-room, original code; no GPL source consulted (CONTRIBUTING.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace meridian::characters {

// Canonical M0 roster ids (append-only; the id stored in character.race/class and
// sent by the client mirror). Provided as named constants for server-side tests
// and tooling — the RUNTIME source of truth is the loaded `Roster` below, whose
// pack entries (Ardent/Dolmen races, Vanguard/Warden classes) come from the pack
// and whose fallback entries (Sylvane/Emberkin races, Runcaller/Mender classes)
// come from this file until SP5 authors them in-pack.
inline constexpr std::uint8_t kRaceArdent   = 1;  // pack (source of truth)
inline constexpr std::uint8_t kRaceDolmen   = 2;  // pack
inline constexpr std::uint8_t kRaceSylvane  = 3;  // compiled fallback (not yet in pack)
inline constexpr std::uint8_t kRaceEmberkin = 4;  // compiled fallback

inline constexpr std::uint8_t kClassVanguard  = 1;  // pack (source of truth)
inline constexpr std::uint8_t kClassRuncaller = 2;  // compiled fallback (not yet in pack)
inline constexpr std::uint8_t kClassWarden    = 3;  // pack
inline constexpr std::uint8_t kClassMender    = 4;  // compiled fallback

// Number of ids in the full M0 roster (races / classes). The compiled M0 set is
// contiguous [1, count]; a valid roster id is not required to be contiguous in
// general (the pack may extend the set with gaps), but this bound is handy for
// tests that need a definitely-out-of-range id.
inline constexpr std::uint8_t kRaceCount  = 4;
inline constexpr std::uint8_t kClassCount = 4;

// The runtime playable roster: the set of valid race/class ids + their display
// names, loaded from pack data and merged with the compiled fallback. Character
// CREATE validates against this (is_valid_race / is_valid_class). Copyable/movable
// (small — a handful of entries); worldd loads one at boot and hands a const& to
// the create path.
class Roster {
public:
    Roster() = default;

    // Insert or REPLACE a race/class entry. The pack load calls these on top of a
    // fallback copy, so a pack entry supersedes a same-id fallback entry (the pack
    // is the source of truth). id 0 is rejected (reserved as unset/invalid).
    void add_race(std::uint8_t id, std::string name);
    void add_class(std::uint8_t id, std::string name);

    // True iff `id` is a defined race/class id in this roster (rejects 0 and any
    // id not present). Replaces the old free is_valid_race / is_valid_class.
    bool is_valid_race(std::uint8_t id) const { return races_.count(id) != 0; }
    bool is_valid_class(std::uint8_t id) const { return classes_.count(id) != 0; }

    // Display name for a race/class id (diagnostics/logging only). "" for unknown.
    std::string_view race_name(std::uint8_t id) const;
    std::string_view class_name(std::uint8_t id) const;

    std::size_t race_count() const { return races_.size(); }
    std::size_t class_count() const { return classes_.size(); }

    // The thin COMPILED FALLBACK — ONLY the four M0 entries not yet authorable in
    // the pack (Sylvane/Emberkin races, Runcaller/Mender classes). This is the base
    // the worldd pack load merges the real pack entries onto. Kept as a shared
    // read-only instance (transitional; shrinks to empty when SP5 lands).
    static const Roster& compiled_fallback();

    // A full OFFLINE M0 roster = compiled_fallback() ∪ a mirror of the seed pack's
    // four entries (Ardent/Dolmen/Vanguard/Warden). For DB-LESS callers that still
    // need to validate the full M0 set without loading the pack from a world DB:
    // the characters unit test, the meridian-character CLI, and the DB-less worldd
    // dispatch smoke path. At runtime with a world DB, worldd instead loads the
    // roster from pack data (compiled_fallback() ∪ the real `race`/`class` rows) —
    // the pack, not this mirror, is the source of truth. Analogous to the client's
    // hand-kept MeridianRoster mirror.
    static const Roster& offline_full();

private:
    std::map<std::uint8_t, std::string> races_;
    std::map<std::uint8_t, std::string> classes_;
};

}  // namespace meridian::characters
