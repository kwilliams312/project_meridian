// SPDX-License-Identifier: Apache-2.0
//
// worldd — the effective-stat framework (SP2.4 #694). Implementation of
// effective_stats.h. CLEAN-ROOM from SP2 design §2.3 + the pack attribute
// vocabulary + the existing worldd headers — see the header for the full
// provenance note. No GPL / emulator source consulted (CONTRIBUTING.md).

#include "effective_stats.h"

#include <algorithm>
#include <cstdlib>

namespace meridian::worldd {

namespace {

// The percent unit is hundredths of a percent-point, so a full multiplier of 1.0
// is 10000 units. effective = flat_total * (10000 + percent) / 10000, rounded
// half-away-from-zero in integer arithmetic (deterministic across platforms — no
// float rounding-mode dependence).
constexpr std::int64_t kPercentScale = 10000;

std::int32_t apply_percent(std::int32_t flat_total, std::int32_t percent) {
    if (percent == 0) return flat_total;  // exact — no rounding needed
    const std::int64_t scaled =
        static_cast<std::int64_t>(flat_total) * (kPercentScale + percent);
    std::int64_t q = scaled / kPercentScale;
    const std::int64_t r = scaled % kPercentScale;  // sign matches `scaled`
    if (std::llabs(r) * 2 >= kPercentScale) q += (scaled >= 0 ? 1 : -1);
    return static_cast<std::int32_t>(q);
}

std::int32_t mod_lookup(
    const std::unordered_map<std::uint8_t, std::unordered_map<std::string, std::int32_t>>& mods,
    std::uint8_t roster_id, const std::string& attr_ref) {
    const auto it = mods.find(roster_id);
    if (it == mods.end()) return 0;
    const auto jt = it->second.find(attr_ref);
    return jt == it->second.end() ? 0 : jt->second;
}

}  // namespace

// ---------------------------------------------------------------------------
// AttributeCatalog
// ---------------------------------------------------------------------------

void AttributeCatalog::add_attribute(AttributeDef def) {
    const std::string ref = def.ref;  // key before the move
    attributes_[ref] = std::move(def);
}

void AttributeCatalog::add_class_mod(std::uint8_t class_roster_id,
                                     const std::string& attr_ref, std::int32_t value) {
    class_mods_[class_roster_id][attr_ref] = value;
}

void AttributeCatalog::add_race_mod(std::uint8_t race_roster_id,
                                    const std::string& attr_ref, std::int32_t value) {
    race_mods_[race_roster_id][attr_ref] = value;
}

const AttributeDef* AttributeCatalog::find(const std::string& ref) const {
    const auto it = attributes_.find(ref);
    return it == attributes_.end() ? nullptr : &it->second;
}

bool AttributeCatalog::is_primary(const std::string& ref) const {
    const AttributeDef* d = find(ref);
    return d != nullptr && d->kind == AttributeKind::kPrimary;
}

std::int32_t AttributeCatalog::class_mod(std::uint8_t class_roster_id,
                                         const std::string& attr_ref) const {
    return mod_lookup(class_mods_, class_roster_id, attr_ref);
}

std::int32_t AttributeCatalog::race_mod(std::uint8_t race_roster_id,
                                        const std::string& attr_ref) const {
    return mod_lookup(race_mods_, race_roster_id, attr_ref);
}

std::size_t AttributeCatalog::primary_count() const {
    std::size_t n = 0;
    for (const auto& [ref, def] : attributes_)
        if (def.kind == AttributeKind::kPrimary) ++n;
    return n;
}

std::size_t AttributeCatalog::derived_count() const {
    return attributes_.size() - primary_count();
}

std::vector<AttributeDef> AttributeCatalog::attributes() const {
    std::vector<AttributeDef> out;
    out.reserve(attributes_.size());
    for (const auto& [ref, def] : attributes_) out.push_back(def);
    // Deterministic order: by content_id, then ref (a stable tie-break for the
    // legacy/seed 0 content_id or any duplicate id from a malformed load).
    std::sort(out.begin(), out.end(), [](const AttributeDef& a, const AttributeDef& b) {
        if (a.content_id != b.content_id) return a.content_id < b.content_id;
        return a.ref < b.ref;
    });
    return out;
}

// ---------------------------------------------------------------------------
// EffectiveStats
// ---------------------------------------------------------------------------

void EffectiveStats::set_base(const std::string& attr_ref, std::int32_t value) {
    base_[attr_ref] = value;
}

std::int32_t EffectiveStats::base(const std::string& attr_ref) const {
    const auto it = base_.find(attr_ref);
    return it == base_.end() ? 0 : it->second;
}

std::int32_t EffectiveStats::static_value(const std::string& attr_ref) const {
    return base(attr_ref) + catalog_.class_mod(class_id_, attr_ref) +
           catalog_.race_mod(race_id_, attr_ref);
}

std::int32_t EffectiveStats::effective(const std::string& attr_ref,
                                       const AttributeDelta& delta) const {
    const std::int32_t flat_total = static_value(attr_ref) + delta.flat;
    return apply_percent(flat_total, delta.percent);
}

std::int32_t EffectiveStats::effective(const std::string& attr_ref,
                                       const AuraContainer& auras) const {
    return effective(attr_ref, auras.attribute_delta(attr_ref));
}

EffectiveStat EffectiveStats::breakdown(const std::string& attr_ref,
                                        const AttributeDelta& delta) const {
    EffectiveStat s;
    s.base = base(attr_ref);
    s.flat_mods = catalog_.class_mod(class_id_, attr_ref) +
                  catalog_.race_mod(race_id_, attr_ref) + delta.flat;
    s.percent = delta.percent;
    s.value = apply_percent(s.base + s.flat_mods, s.percent);
    return s;
}

EffectiveStat EffectiveStats::breakdown(const std::string& attr_ref,
                                        const AuraContainer& auras) const {
    return breakdown(attr_ref, auras.attribute_delta(attr_ref));
}

// ---------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------

const char* attribute_kind_name(AttributeKind k) {
    switch (k) {
        case AttributeKind::kPrimary: return "primary";
        case AttributeKind::kDerived: return "derived";
    }
    return "?";
}

}  // namespace meridian::worldd
