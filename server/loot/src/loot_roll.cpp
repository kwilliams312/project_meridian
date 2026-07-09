// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — the deterministic seeded loot roll (ITM-02; issue #369).
// See loot_roll.h for the design + clean-room provenance.

#include "loot_roll.h"

namespace meridian::loot {

namespace {

// Pick one entry from a group by relative weight, using one bounded roll over the
// summed weights. Entries with weight 0 are unselectable (never chosen). Returns
// nullptr only if the group has no positively-weighted entry.
const LootEntry* pick_weighted(const LootGroup& group, LootRng& rng) {
    std::uint32_t total = 0;
    for (const LootEntry& e : group.entries) total += e.weight;
    if (total == 0) return nullptr;

    std::uint32_t r = rng.roll_below(total);  // [0, total)
    for (const LootEntry& e : group.entries) {
        if (e.weight == 0) continue;
        if (r < e.weight) return &e;
        r -= e.weight;
    }
    return nullptr;  // unreachable (r < total), defensive
}

}  // namespace

LootRoll roll_loot(const LootTable& table, LootRng& rng) {
    LootRoll out;

    // Money first (one amount roll over the copper range). A 0/0 range yields 0.
    const std::uint32_t money = rng.roll_amount(
        static_cast<std::uint32_t>(table.money_min < 0 ? 0 : table.money_min),
        static_cast<std::uint32_t>(table.money_max < 0 ? 0 : table.money_max));
    out.copper = static_cast<items::Copper>(money);

    // Each group in order: one chance roll; on a hit, one weighted selection, then
    // one quantity roll. Fixed draw order keeps the sequence stable/auditable.
    for (const LootGroup& g : table.groups) {
        const bool hit = rng.roll_bp() < g.chance_bp;
        if (!hit) continue;
        const LootEntry* e = pick_weighted(g, rng);
        if (e == nullptr) continue;
        const std::uint32_t qty = rng.roll_amount(e->min_qty, e->max_qty);
        if (qty == 0) continue;  // a degenerate 0-quantity entry drops nothing
        out.stacks.push_back(
            LootStack{e->item_template_id, qty, e->required_quest_id});
    }

    return out;
}

}  // namespace meridian::loot
