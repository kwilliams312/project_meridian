// SPDX-License-Identifier: Apache-2.0
//
// worldd — the EFFECTIVE-STAT framework (SP2.4 #694, epic #690; SP2 design §2.3).
//
// WHAT THIS FILE IS: the kernel's attribute framework — the single view that turns
// a character's BASE attributes plus the tuning layers into an EFFECTIVE value the
// combat formulas and the buff/debuff primitives read/write. It unifies three
// contributions the earlier stories left in separate places:
//
//   effective(attr) = round( ( base(attr)                       // per-character base
//                              + class_mod(attr)                 // per-class attribute_mods (pack)
//                              + race_mod(attr)                  // per-race  attribute_mods (pack)
//                              + aura_flat(attr) )               // live buff/debuff FLAT (aura layer)
//                            * (1 + aura_percent(attr)/10000) )  // live buff/debuff PERCENT (aura layer)
//
// The kernel ships the base attribute VOCABULARY (primary str/agi/sta/int/spi +
// derived crit/haste/armor — SP1's meridian/attribute@1 seeds); OPERATORS tune the
// per-class/per-race mods (chibi races zero theirs). Adding brand-new attributes
// stays a later extension (umbrella §5) — this framework consumes the fixed
// vocabulary, it never invents one.
//
// ─── WHAT IT REPLACES (SP2.3 #693's interim ledger seam) ─────────────────────
// #693 parked live buff/debuff modifiers on AuraContainer's interim ledger
// (attr_flat_ / attr_percent_, exposed via AuraContainer::attribute_delta()): a
// PRIMARY flat modifier also mirrored onto the StatKey layer (stat_delta()), but
// PERCENT modifiers and DERIVED attributes had no consumer — they were merely
// parked "for #694". THIS is #694: EffectiveStats is that consumer. It reads the
// aura layer through AttributeDelta (flat + percent, primary AND derived) and folds
// it with the base + pack mods into one effective value. Primary and derived,
// flat and percent, all resolve through the SAME effective() call — the "one
// effective-stat view" the spec asks for.
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE: like aura_container + combat_resolver,
// this is plain deterministic arithmetic over in-memory data. The AttributeCatalog
// is filled once at boot from the world DB (db_content_store::load_db_attributes);
// the math here touches no DB, socket, or clock and runs in the plain `server`
// ctest with no MariaDB.
//
// CLEAN-ROOM: designed from SP2 design §2.3 (2026-07-13-sp2-kernel-class-character-
// system-design.md), the pack-contract attribute vocabulary
// (schema/content/attribute.schema.yaml + common.defs.yaml $defs/attributeMods),
// and the existing worldd headers (ability_store StatKey / aura_container
// AttributeDelta). Every rule is ORIGINAL — no GPL / AGPL / CMaNGOS / TrinityCore /
// leaked emulator source consulted (no existing stat/aura system). See CONTRIBUTING.md.

#ifndef MERIDIAN_WORLDD_EFFECTIVE_STATS_H
#define MERIDIAN_WORLDD_EFFECTIVE_STATS_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "aura_container.h"  // AttributeDelta / AuraContainer — the live buff/debuff layer

namespace meridian::worldd {

// The kernel-blessed kind of an attribute — mirrors meridian/attribute@1's
// `kind` ENUM('primary','derived'). PRIMARY attributes (str/agi/sta/int/spi) also
// map onto ability_store's StatKey; DERIVED attributes (crit/haste/armor) live
// ONLY in this framework (no StatKey), which is exactly why #694 has to exist —
// before it, a derived buff had nowhere to be read.
enum class AttributeKind : std::uint8_t {
    kPrimary,
    kDerived,
};

// One attribute definition (meridian/attribute@1), keyed by its contentId ref (the
// SAME verbatim ref the aura ledger + the pack attribute_mods use — e.g.
// "core:attribute.strength"), so every layer joins on one string.
struct AttributeDef {
    std::string   ref;                 // contentId ("core:attribute.strength")
    std::string   name;                // displayName ("Strength")
    AttributeKind kind = AttributeKind::kPrimary;
    std::uint32_t content_id = 0;      // IF-9 numeric id (traceability)
};

// ---------------------------------------------------------------------------
// AttributeCatalog — the loaded attribute vocabulary + the per-class/per-race
// attribute_mods, read-only after boot.
// ---------------------------------------------------------------------------
//
// Filled from the world DB `attribute` / `class_attribute_mod` / `race_attribute_mod`
// tables (mcc emit-sql walks the attribute entities + each race/class's
// attribute_mods; db_content_store::load_db_attributes reads them). Mods are keyed
// by the canonical roster_id (character.class / character.race), matching the
// runtime Roster, so a character's class/race id looks its mods up directly.
class AttributeCatalog {
public:
    AttributeCatalog() = default;

    // Register one attribute definition (idempotent replace by ref).
    void add_attribute(AttributeDef def);
    // Add a per-class / per-race flat modifier of `value` to `attr_ref`. A later
    // add for the same (id, ref) replaces the earlier value (the DDL PK forbids
    // duplicates, so this only guards a malformed double-load).
    void add_class_mod(std::uint8_t class_roster_id, const std::string& attr_ref,
                       std::int32_t value);
    void add_race_mod(std::uint8_t race_roster_id, const std::string& attr_ref,
                      std::int32_t value);

    // The definition for `ref`, or nullptr when the ref is not a known attribute.
    const AttributeDef* find(const std::string& ref) const;
    // Whether `ref` is a defined PRIMARY attribute (false for derived OR unknown).
    bool is_primary(const std::string& ref) const;

    // The signed flat modifier this class/race contributes to `attr_ref` (0 when
    // the class/race has no mod for it — the common case).
    std::int32_t class_mod(std::uint8_t class_roster_id, const std::string& attr_ref) const;
    std::int32_t race_mod(std::uint8_t race_roster_id, const std::string& attr_ref) const;

    std::size_t attribute_count() const { return attributes_.size(); }
    std::size_t primary_count() const;
    std::size_t derived_count() const;

    // Every registered attribute definition, ordered by content_id (then ref for a
    // stable tie-break) so a consumer that walks the vocabulary — e.g. the
    // per-character aggregator (effective_stats_aggregator.h) computing an effective
    // value for each attribute — gets a deterministic order independent of the
    // unordered_map's bucketing. Read-only view; cheap to build (the catalog is
    // small and boot-time immutable).
    std::vector<AttributeDef> attributes() const;

private:
    // ref -> definition.
    std::unordered_map<std::string, AttributeDef> attributes_;
    // roster_id -> (attr_ref -> value).
    std::unordered_map<std::uint8_t, std::unordered_map<std::string, std::int32_t>> class_mods_;
    std::unordered_map<std::uint8_t, std::unordered_map<std::string, std::int32_t>> race_mods_;
};

// The full breakdown of one attribute's effective value — so a caller (combat
// formula, tooling, tests) can see WHERE the number came from, not just the total.
struct EffectiveStat {
    std::int32_t base = 0;       // the character's base value for the attribute
    std::int32_t flat_mods = 0;  // class_mod + race_mod + aura FLAT (all summed)
    std::int32_t percent = 0;    // aura PERCENT (hundredths of a percent-point)
    std::int32_t value = 0;      // the final effective value (see effective())
};

// ---------------------------------------------------------------------------
// EffectiveStats — a character's live effective-stat view.
// ---------------------------------------------------------------------------
//
// Bound to (catalog, race_id, class_id) for the character's lifetime. Holds the
// character's BASE attribute values (set_base) — the class/race mods come from the
// catalog and the live buff/debuff layer is passed in at query time (either as a
// raw AttributeDelta, or read out of an AuraContainer). Copyable/small.
//
// PERCENT UNIT: aura percent is in hundredths of a percent-point (the schema's
// `modifier: percent` unit, mirrored in aura_container's AttributeDelta) — a
// percent of 500 means +5.00%%, i.e. multiply the flat total by 1.05. The multiply
// happens in integer-friendly form and rounds half-away-from-zero.
class EffectiveStats {
public:
    EffectiveStats(const AttributeCatalog& catalog, std::uint8_t race_roster_id,
                   std::uint8_t class_roster_id)
        : catalog_(catalog), race_id_(race_roster_id), class_id_(class_roster_id) {}

    // Set the character's BASE value for `attr_ref` (before any layering). A base
    // not set is treated as 0 (derived attributes typically start at 0 and are
    // built up entirely by mods/buffs; primaries carry a per-character base).
    void set_base(const std::string& attr_ref, std::int32_t value);
    std::int32_t base(const std::string& attr_ref) const;

    // The STATIC value = base + class_mod + race_mod (NO live auras). This is the
    // character's effective stat at rest; combat uses effective() below with the
    // live aura layer folded in.
    std::int32_t static_value(const std::string& attr_ref) const;

    // The EFFECTIVE value with a live buff/debuff layer `delta` folded in:
    //   round( (static_value + delta.flat) * (1 + delta.percent/10000) ).
    // This is the framework's core: base + pack mods + aura flat, scaled by aura
    // percent — for a PRIMARY or a DERIVED attribute alike.
    std::int32_t effective(const std::string& attr_ref, const AttributeDelta& delta) const;

    // Convenience: read the live layer out of `auras` (AuraContainer::attribute_delta
    // — the SP2.3 interim ledger this framework consumes) and fold it in. Equivalent
    // to effective(ref, auras.attribute_delta(ref)).
    std::int32_t effective(const std::string& attr_ref, const AuraContainer& auras) const;

    // The full breakdown (base / flat_mods / percent / value) for `attr_ref` given
    // the live layer — the same math as effective(), exposed for diagnostics/tests.
    EffectiveStat breakdown(const std::string& attr_ref, const AttributeDelta& delta) const;
    EffectiveStat breakdown(const std::string& attr_ref, const AuraContainer& auras) const;

private:
    const AttributeCatalog& catalog_;
    std::uint8_t race_id_ = 0;
    std::uint8_t class_id_ = 0;
    std::unordered_map<std::string, std::int32_t> base_;
};

// Human-readable enum name (logs / tooling / test diagnostics; not the hot path).
const char* attribute_kind_name(AttributeKind k);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_EFFECTIVE_STATS_H
