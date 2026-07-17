// SPDX-License-Identifier: Apache-2.0
//
// worldd — implementation of the pure per-character effective-stat aggregator
// (SP2.5 #896). CLEAN-ROOM from the #694 kernel + the ITM-01 template model — see
// the header for the full provenance note. No GPL/emulator source consulted
// (CONTRIBUTING.md).

#include "effective_stats_aggregator.h"

#include <string>

namespace meridian::worldd {

namespace {

namespace itm = meridian::items;

// The leaf token of an attribute contentId ref ("core:attribute.strength" ->
// "strength"), matching the SP2.3 primary_attribute_stat convention: the primary
// vocabulary is fixed by the kernel and keyed off the FINAL dot-segment, so any
// namespace resolves.
std::string ref_leaf(const std::string& ref) {
    const std::string::size_type dot = ref.find_last_of('.');
    return dot == std::string::npos ? ref : ref.substr(dot + 1);
}

// The attribute leaf a gear StatKey (items vocabulary) contributes to — the bridge
// from the item-template stat vocabulary (items::StatKey) to the attribute-ref
// vocabulary the catalog/kernel use. Mirrors the five M1 primaries.
const char* stat_key_leaf(itm::StatKey k) {
    switch (k) {
        case itm::StatKey::kStrength:  return "strength";
        case itm::StatKey::kAgility:   return "agility";
        case itm::StatKey::kStamina:   return "stamina";
        case itm::StatKey::kIntellect: return "intellect";
        case itm::StatKey::kSpirit:    return "spirit";
    }
    return "";
}

}  // namespace

std::int32_t AggregatedCharacterStats::attribute(const std::string& attr_ref) const {
    const auto it = attributes.find(attr_ref);
    return it == attributes.end() ? 0 : it->second;
}

AggregatedCharacterStats aggregate_character_stats(
    const AttributeCatalog& catalog, std::uint8_t race_roster_id,
    std::uint8_t class_roster_id, std::uint16_t level,
    const std::vector<const itm::ItemTemplate*>& equipped) {
    AggregatedCharacterStats out;
    out.level = level;

    // --- 1. Sum the equipped gear: primary StatMods (by leaf) + armor. --------
    // Gear contributes ONLY to primary attributes (the item StatKey vocabulary is
    // the five primaries); armor is a distinct top-level item field, summed apart.
    std::unordered_map<std::string, std::int32_t> gear_flat;  // attr leaf -> summed flat
    for (const itm::ItemTemplate* tmpl : equipped) {
        if (tmpl == nullptr) continue;  // a paperdoll gap / defensive skip
        for (const itm::StatMod& mod : tmpl->stats) {
            gear_flat[stat_key_leaf(mod.stat)] += mod.amount;
        }
        out.gear_armor += static_cast<std::int32_t>(tmpl->armor);
    }

    // --- 2. Fold each attribute through the #694 kernel. ----------------------
    // Zero-base design (see header): set_base is never called, so the kernel's base
    // is 0 and static_value == class_mod + race_mod. Gear flat for a matching
    // primary is passed as the AttributeDelta.flat, so the aggregator's number is,
    // by construction, EffectiveStats::effective(ref, {gear_flat, 0}) — it cannot
    // diverge from the kernel.
    EffectiveStats kernel(catalog, race_roster_id, class_roster_id);
    for (const AttributeDef& def : catalog.attributes()) {
        AttributeDelta delta;  // percent stays 0 — no live auras in this static snapshot
        const auto git = gear_flat.find(ref_leaf(def.ref));
        if (git != gear_flat.end()) delta.flat = git->second;
        out.attributes[def.ref] = kernel.effective(def.ref, delta);
    }

    return out;
}

}  // namespace meridian::worldd
