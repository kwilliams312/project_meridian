// SPDX-License-Identifier: Apache-2.0
//
// worldd — the TALENT catalog + talent-application hook (SP2.7 #697, epic #690;
// SP2 design §2.4/§2.5). The kernel loads a class's talent tree (tiers unlocked by
// spent points) and the talents it awards, then APPLIES a character's talent grants:
//   * ability grants  -> the ability ids the character's talents make usable, and
//   * passive stat grants (buff/debuff over an attribute id) -> an AttributeDelta
//     layer the SP2.4 effective-stat framework folds in (EffectiveStats::effective
//     accepts an AttributeDelta) — REUSING that framework, not duplicating it.
//
// SCOPE (minimal + server-authoritative, per the story): talent POINT allocation +
// persistence is a larger character-progression system not yet built, so this hook
// does not invent one. It exposes apply_talents(tree, points_spent) — a PURE
// function that, given a hypothetical spent-point total, computes the granted
// abilities + the passive attribute deltas from the unlocked tiers. worldd calls it
// once the character-progression story wires spent points; the DB-backed test drives
// it directly to prove the grants land (an ability id becomes granted; a passive
// buff raises the right effective attribute through the SP2.4 framework).
//
// PURE / DB-FREE: like class_kernel + effective_stats, plain deterministic data +
// arithmetic. The catalog is filled once at boot from the world DB
// (db_content_store::load_db_talents); this logic touches no DB/socket/clock.
//
// CLEAN-ROOM: designed from SP2 design §2.4/§2.5 + the talent / talent_tree content
// schemas; no GPL/AGPL/CMaNGOS/TrinityCore/leaked source consulted (CONTRIBUTING.md).

#ifndef MERIDIAN_WORLDD_TALENT_CATALOG_H
#define MERIDIAN_WORLDD_TALENT_CATALOG_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ability_store.h"   // meridian::worldd::AttributeModifier (flat/percent)
#include "aura_container.h"   // meridian::worldd::AttributeDelta (the SP2.4 layer)

namespace meridian::worldd {

// One grant a talent awards — a tagged union by `kind` (mirrors talent.schema.yaml
// grants[]). An `ability` grant names an ability id the talent unlocks; a `buff` /
// `debuff` grant is a PASSIVE stat effect over an attribute ref.
struct TalentGrant {
    enum class Kind : std::uint8_t { kAbility, kBuff, kDebuff };
    Kind kind = Kind::kAbility;
    // kAbility:
    std::uint32_t ability_id = 0;
    // kBuff / kDebuff:
    std::string       attribute_ref;                          // attribute contentId
    std::int32_t      amount = 0;                             // signed modifier
    AttributeModifier modifier = AttributeModifier::kFlat;    // flat | percent
    std::uint32_t     duration_ms = 0;                       // 0 = permanent passive
    std::uint16_t     max_stacks = 1;
};

// One talent (meridian/talent@1), keyed by its IF-9 numeric id.
struct TalentDef {
    std::uint32_t content_id = 0;
    std::string   ref;                    // verbatim contentId (core:talent.battle_fury)
    std::string   name;
    std::uint16_t rank_max = 1;
    std::vector<TalentGrant> grants;      // ordered by ordinal
};

// One tier of a talent tree — the talents it offers, unlocked at `required_points`.
struct TalentTreeTier {
    std::uint32_t required_points = 0;
    std::vector<std::uint32_t> talent_ids;  // talent numeric ids, in tier order
};

// One talent tree (meridian/talent_tree@1), keyed by its IF-9 numeric id.
struct TalentTreeDef {
    std::uint32_t content_id = 0;
    std::string   ref;                    // verbatim contentId (core:talent_tree.vanguard_path)
    std::string   name;
    std::vector<TalentTreeTier> tiers;    // ordered by tier_ordinal (ascending threshold)
};

// ---------------------------------------------------------------------------
// TalentCatalog — the loaded talents + talent trees, read-only after boot.
// ---------------------------------------------------------------------------
class TalentCatalog {
public:
    TalentCatalog() = default;

    // Get-or-create by numeric id (the loaders build each up from child rows).
    TalentDef&     touch_talent(std::uint32_t content_id);
    TalentTreeDef& touch_tree(std::uint32_t content_id);

    const TalentDef*     find_talent(std::uint32_t content_id) const;
    const TalentTreeDef* find_tree(std::uint32_t content_id) const;

    std::size_t talent_count() const { return talents_.size(); }
    std::size_t tree_count() const { return trees_.size(); }

private:
    std::unordered_map<std::uint32_t, TalentDef>     talents_;
    std::unordered_map<std::uint32_t, TalentTreeDef> trees_;
};

// The result of applying a character's talent grants: the ability ids the talents
// make usable, and the net PASSIVE attribute deltas (keyed by attribute ref — the
// SAME key the aura ledger + class/race mods use) to fold into EffectiveStats.
struct TalentApplication {
    std::vector<std::uint32_t> granted_ability_ids;                // ascending, de-duplicated
    std::unordered_map<std::string, AttributeDelta> passive_by_attr;  // ref -> {flat, percent}

    // Convenience: the net passive delta for one attribute (zero when none).
    AttributeDelta passive(const std::string& attr_ref) const;
};

// Apply every talent in `tree`'s tiers whose required_points <= `points_spent`
// (row-unlock by points, spec §2.5), accumulating each talent's grants. Only
// PERMANENT passives (duration_ms == 0) fold into the effective-stat layer — a
// timed proc is an aura the combat system applies, not a standing talent bonus.
// Unknown talent ids in a tier are skipped (a malformed tree degrades, never
// throws). `catalog` resolves the tier's talent ids to their grants.
TalentApplication apply_talents(const TalentTreeDef& tree, const TalentCatalog& catalog,
                                std::uint32_t points_spent);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_TALENT_CATALOG_H
