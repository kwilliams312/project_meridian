// SPDX-License-Identifier: Apache-2.0
//
// worldd — the PURE per-character effective-stat AGGREGATOR (SP2.5 #896; epic #866
// S5a; split from #871 per the 2026-07-17 scout).
//
// WHAT THIS FILE IS: the single, thread-agnostic function that turns a character's
// static identity (class/race/level) plus an equipped-gear loadout into ONE
// snapshot of effective stats — the shared seam #871/S5b (the CHARACTER_STATS push)
// and #785 (combat's set_effective_armor) both consume, without either being built
// here. It layers ONLY over the #694 EffectiveStats kernel (effective_stats.h): the
// gear's per-item StatMods become a FLAT AttributeDelta folded through the SAME
// EffectiveStats::effective() the kernel already ships, so the aggregator can never
// silently diverge from the kernel's math (base + class_mod + race_mod + flat).
//
// SCOPE FENCE (deliberately narrow — this is what keeps #896 off #785/#897/#898):
//   * NO wire / opcode / world.fbs        — that is S5b (#897).
//   * NO Unit mutation / set_effective_armor / combat — that is #785.
//   * NO client                           — that is S5c (#898).
//   * NO world-thread coupling, NO DB, NO socket, NO clock — a plain function,
//     callable from anywhere (the IO thread, a test, a tool).
//   * Does NOT touch health derivation (placeholder_player_stats' class curve).
//
// BASE-ATTRIBUTE SOURCE (a small in-scope design choice, flagged for review):
// LoadedCharacter carries no per-character base attributes, so this aggregator uses
// a ZERO base for every attribute and lets the class/race attribute_mods supply the
// starting values — exactly how EffectiveStats::static_value already composes a stat
// when set_base is never called (base defaults to 0). No speculative per-level curve
// is invented; `level` is carried on the snapshot for consumers (S5b displays it,
// and a future level curve — likely tied to health, out of scope here — can key off
// it) but does NOT itself scale any attribute yet.
//
// GEAR ARMOR is surfaced as a DISTINCT output (gear_armor), NOT folded into the
// derived "armor" attribute: #785 later feeds it to combat's effective-armor and
// S5b displays it separately, so mixing item armor into the attribute layer would
// destroy information both consumers need apart.
//
// CLEAN-ROOM: composed from the #694 kernel (effective_stats.h), the ITM-01 item
// template model (server/items/src/item_template.h StatMod/armor), and the SP2.3
// ref→StatKey leaf convention (ability_store.cpp primary_attribute_stat). Every
// rule is ORIGINAL — no GPL/AGPL/CMaNGOS/TrinityCore/leaked emulator source
// consulted (no existing stat/aggregation system). See CONTRIBUTING.md.

#ifndef MERIDIAN_WORLDD_EFFECTIVE_STATS_AGGREGATOR_H
#define MERIDIAN_WORLDD_EFFECTIVE_STATS_AGGREGATOR_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "effective_stats.h"
#include "item_template.h"  // meridian::items::ItemTemplate / StatMod / StatKey

namespace meridian::worldd {

// A character's aggregated effective-stat snapshot — the output of the aggregator.
// PURE DATA: no ownership of the catalog or the templates, safe to copy/move.
struct AggregatedCharacterStats {
    // The level the snapshot was computed at (echoed input; see the base-attribute
    // note above — it does not yet scale attributes, but consumers surface it).
    std::uint16_t level = 0;

    // Effective value per attribute contentId ref — PRIMARY and DERIVED alike,
    // every attribute the catalog defines. Primary attributes include the summed
    // gear StatMod contribution; derived attributes are base(0)+class+race (no item
    // StatMod feeds a derived attribute in the M1 vocabulary). Keyed by the SAME ref
    // string the catalog / aura ledger use, so consumers join on one string.
    std::unordered_map<std::string, std::int32_t> attributes;

    // Summed armor of every equipped item (item_template.armor) — kept DISTINCT
    // from the derived "armor" attribute so #785 can feed it to combat and S5b can
    // display it on its own.
    std::int32_t gear_armor = 0;

    // Effective value for `attr_ref`, or 0 when the ref is not a defined attribute.
    std::int32_t attribute(const std::string& attr_ref) const;
};

// Aggregate a character's effective stats from its static identity + equipped gear.
//
//   catalog          the boot-loaded attribute vocabulary + per-class/race mods
//   race_roster_id   the character's race roster id (catalog race_mod key)
//   class_roster_id  the character's class roster id (catalog class_mod key)
//   level            the character's level (carried on the snapshot; see note)
//   equipped         the currently-equipped item templates (nullptrs are skipped)
//
// Returns the snapshot: an effective value for every catalog attribute (gear
// StatMods folded into the matching primary attribute via the #694 kernel) plus the
// summed gear armor. PURE — no DB/socket/clock/thread; the equipped set is the sole
// gear input, so re-aggregating after an equip change inherently drops a removed
// item's contribution exactly once (there is no accumulated state to unwind).
AggregatedCharacterStats aggregate_character_stats(
    const AttributeCatalog& catalog, std::uint8_t race_roster_id,
    std::uint8_t class_roster_id, std::uint16_t level,
    const std::vector<const meridian::items::ItemTemplate*>& equipped);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_EFFECTIVE_STATS_AGGREGATOR_H
