// SPDX-License-Identifier: Apache-2.0
//
// meridian-loot — the deterministic, SEEDED loot roll (ITM-02; issue #369).
//
// Turns a LootTable (loot_table.h) into a concrete LootRoll — the set of item
// stacks and the copper a corpse drops — using a seeded RNG so a given (seed,
// table) yields the SAME result on every platform. This mirrors the combat
// track's seeded-roll discipline (server SAD §2.5 "seeded rolls"; the worldd
// CombatRng): loot outcomes are server-authoritative and reproducible, so the
// "deterministic roll for a seed" acceptance (#369) and the statistical drop-rate
// tests (server PRD §7 "loot rolls — statistical tests over 10^5 draws") are both
// possible against a fixed seed.
//
// PURE / DB-FREE / CLOCK-FREE: like CombatRng, this reads no wall clock, touches
// no socket/DB, and draws only its own seeded engine. Quest gating is NOT applied
// here — the roll produces every drop the table rolls (each quest stack tagged
// with its required quest); the loot SESSION (loot_session.h) enforces per-looter
// quest eligibility at loot time. Keeping the roll quest-independent makes it a
// pure function of (seed, table), which is what the determinism guarantee needs.
//
// Clean-room, original code (CONTRIBUTING.md).

#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "item_template.h"  // meridian::items::Copper
#include "loot_table.h"

namespace meridian::loot {

// Deterministic, seeded RNG for all loot rolls (mirrors worldd CombatRng — same
// mt19937_64 engine + basis-point convention). A separate type (not CombatRng)
// so the loot library stays independent of worldd, and so a map's loot rolls draw
// from their OWN stream — never perturbing the combat RNG sequence (which would
// shift byte-stable combat event streams).
class LootRng {
public:
    explicit LootRng(std::uint64_t seed) : engine_(seed) {}

    // A uniform roll in [0, kLootRollScale) — the d10000 chance roll. A drop with
    // chance `bp` hits when roll_bp() < bp.
    std::uint32_t roll_bp() {
        return static_cast<std::uint32_t>(engine_() % kLootRollScale);
    }

    // A uniform integer in [0, bound) (bound > 0) — weighted-entry selection.
    std::uint32_t roll_below(std::uint32_t bound) {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(engine_() % bound);
    }

    // A uniform amount in [lo, hi] inclusive (quantity / money range). lo when
    // hi <= lo (a fixed amount).
    std::uint32_t roll_amount(std::uint32_t lo, std::uint32_t hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<std::uint32_t>(
                        engine_() % (static_cast<std::uint64_t>(hi - lo) + 1));
    }

    // Reseed deterministically (e.g. per corpse from its guid).
    void reseed(std::uint64_t seed) { engine_.seed(seed); }

private:
    std::mt19937_64 engine_;
};

// One rolled drop: a concrete quantity of one item template, carrying the quest
// gate (0 = normal) so the loot session can hide a quest stack from an ineligible
// looter. A LootRoll is a small vector of these plus the copper.
struct LootStack {
    std::uint32_t item_template_id = 0;
    std::uint32_t count = 1;                // >= 1
    std::uint32_t required_quest_id = 0;    // 0 = normal drop; else quest-gated

    bool is_quest() const { return required_quest_id != 0; }
};

// The concrete result of rolling one LootTable: the item stacks (in table order —
// group 0's drop first) and the copper. Deterministic for a fixed (seed, table).
struct LootRoll {
    std::vector<LootStack> stacks;
    items::Copper copper = 0;

    bool empty() const { return stacks.empty() && copper == 0; }
};

// Roll `table` with `rng`, producing the drops + money. Deterministic: the SAME
// (rng sequence, table) always yields the SAME LootRoll. Draw order is fixed —
// money first, then each group in order (a chance roll; on a hit a weighted
// selection roll, then a quantity roll) — so the sequence is stable and auditable.
LootRoll roll_loot(const LootTable& table, LootRng& rng);

}  // namespace meridian::loot
