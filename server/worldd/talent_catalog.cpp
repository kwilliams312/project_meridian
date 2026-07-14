// SPDX-License-Identifier: Apache-2.0
//
// worldd — talent catalog + talent-application hook (talent_catalog.h).
// Clean-room, original code (CONTRIBUTING.md).

#include "talent_catalog.h"

#include <algorithm>

namespace meridian::worldd {

TalentDef& TalentCatalog::touch_talent(std::uint32_t content_id) {
    TalentDef& d = talents_[content_id];
    d.content_id = content_id;
    return d;
}

TalentTreeDef& TalentCatalog::touch_tree(std::uint32_t content_id) {
    TalentTreeDef& d = trees_[content_id];
    d.content_id = content_id;
    return d;
}

const TalentDef* TalentCatalog::find_talent(std::uint32_t content_id) const {
    const auto it = talents_.find(content_id);
    return it == talents_.end() ? nullptr : &it->second;
}

const TalentTreeDef* TalentCatalog::find_tree(std::uint32_t content_id) const {
    const auto it = trees_.find(content_id);
    return it == trees_.end() ? nullptr : &it->second;
}

AttributeDelta TalentApplication::passive(const std::string& attr_ref) const {
    const auto it = passive_by_attr.find(attr_ref);
    return it == passive_by_attr.end() ? AttributeDelta{} : it->second;
}

TalentApplication apply_talents(const TalentTreeDef& tree, const TalentCatalog& catalog,
                                std::uint32_t points_spent) {
    TalentApplication out;
    for (const TalentTreeTier& tier : tree.tiers) {
        if (tier.required_points > points_spent) continue;  // tier not yet unlocked
        for (std::uint32_t talent_id : tier.talent_ids) {
            const TalentDef* t = catalog.find_talent(talent_id);
            if (t == nullptr) continue;  // malformed tree — skip, never throw
            for (const TalentGrant& g : t->grants) {
                if (g.kind == TalentGrant::Kind::kAbility) {
                    out.granted_ability_ids.push_back(g.ability_id);
                    continue;
                }
                // A passive buff/debuff — only a PERMANENT one folds into the
                // standing effective-stat layer (a timed proc is an aura the combat
                // system applies, not a standing talent bonus).
                if (g.duration_ms != 0) continue;
                // debuff is a negative-signed buff over the same attribute; the sign
                // is already carried in `amount`, so both fold the same way.
                AttributeDelta& d = out.passive_by_attr[g.attribute_ref];
                if (g.modifier == AttributeModifier::kPercent) {
                    d.percent += g.amount;
                } else {
                    d.flat += g.amount;
                }
            }
        }
    }
    // Deterministic, de-duplicated ability grant list.
    std::sort(out.granted_ability_ids.begin(), out.granted_ability_ids.end());
    out.granted_ability_ids.erase(
        std::unique(out.granted_ability_ids.begin(), out.granted_ability_ids.end()),
        out.granted_ability_ids.end());
    return out;
}

}  // namespace meridian::worldd
