// SPDX-License-Identifier: Apache-2.0
//
// worldd — PURE per-character effective-stat AGGREGATOR unit test (SP2.5 #896;
// epic #866 S5a).
//
// PURE / DB-FREE / WIRE-FREE / UNIT-FREE / CLIENT-FREE: drives
// aggregate_character_stats over an in-memory AttributeCatalog + hand-built
// ItemTemplates — no DB, no socket, no Unit, no clock — so it runs in the PLAIN
// server ctest (no MariaDB). This is the whole point of #896: prove the shared
// stat seam #871/S5b and #785 consume is correct in isolation.
//
// What it proves:
//   A. No gear: effective attribute == base(0) + class_mod + race_mod, i.e. the
//      kernel's static_value — the zero-base design choice, verified against the
//      catalog's own numbers.
//   B. Gear StatMods: item primary-stat mods sum into the RIGHT attributes; an item
//      with no relevant mod leaves an attribute untouched; multiple items stack.
//   C. Gear armor: item armor sums into gear_armor and stays DISTINCT from the
//      derived "armor" attribute (which reflects only class/race mods).
//   D. Unequip / replacement removes the previous contribution EXACTLY once — the
//      #785-relevant invariant proved here so #785 can rely on it.
//   E. Cross-check straight against the EffectiveStats kernel: the aggregator's
//      per-attribute number equals EffectiveStats::effective(ref, {gear_flat,0}),
//      so the aggregator can never silently diverge from the kernel.
//   F. Level is echoed onto the snapshot and does NOT scale attributes (design).
//   G. Robustness: nullptr templates are skipped; unknown ref reads 0.

#include "effective_stats.h"
#include "effective_stats_aggregator.h"
#include "item_template.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace itm = meridian::items;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// The seed attribute vocabulary refs (SP1's meridian/attribute@1).
const std::string kStr = "core:attribute.strength";
const std::string kAgi = "core:attribute.agility";
const std::string kSta = "core:attribute.stamina";
const std::string kInt = "core:attribute.intellect";
const std::string kSpi = "core:attribute.spirit";
const std::string kArmor = "core:attribute.armor";  // derived
const std::string kCrit = "core:attribute.crit";    // derived

// Vanguard (class roster 1): strength +2, stamina +1. A tuned race (roster 2):
// agility +1 and a DERIVED armor +5. Mirrors the #694 kernel unit test's catalog so
// the two tests reason about the same numbers.
AttributeCatalog seed_catalog() {
    AttributeCatalog c;
    c.add_attribute({kStr, "Strength", AttributeKind::kPrimary, 1});
    c.add_attribute({kAgi, "Agility", AttributeKind::kPrimary, 2});
    c.add_attribute({kSta, "Stamina", AttributeKind::kPrimary, 3});
    c.add_attribute({kInt, "Intellect", AttributeKind::kPrimary, 4});
    c.add_attribute({kSpi, "Spirit", AttributeKind::kPrimary, 5});
    c.add_attribute({kArmor, "Armor", AttributeKind::kDerived, 6});
    c.add_attribute({kCrit, "Critical Strike", AttributeKind::kDerived, 7});
    c.add_class_mod(1, kStr, 2);
    c.add_class_mod(1, kSta, 1);
    c.add_race_mod(2, kAgi, 1);
    c.add_race_mod(2, kArmor, 5);
    return c;
}

// A minimal armor template contributing StatMods + an armor value.
itm::ItemTemplate make_item(std::uint32_t id, std::vector<itm::StatMod> stats,
                            std::uint32_t armor) {
    itm::ItemTemplate t;
    t.id = id;
    t.item_class = itm::ItemClass::kArmor;
    t.slot = itm::ItemSlot::kChest;
    t.stats = std::move(stats);
    t.armor = armor;
    return t;
}

}  // namespace

int main() {
    std::printf("effective-stat aggregator unit test (#896)\n");

    const AttributeCatalog cat = seed_catalog();
    // A Vanguard (class 1) of the tuned race (race 2), level 12.
    const std::uint8_t kRace = 2, kClass = 1;
    const std::uint16_t kLevel = 12;

    // --- A. No gear: effective == static (base 0 + class + race) -------------
    {
        const auto s = aggregate_character_stats(cat, kRace, kClass, kLevel, {});
        // strength: 0 base + 2 class + 0 race = 2.
        check("A: strength no-gear = 2 (class mod)", s.attribute(kStr) == 2);
        // agility: 0 + 0 class + 1 race = 1.
        check("A: agility no-gear = 1 (race mod)", s.attribute(kAgi) == 1);
        // stamina: 0 + 1 class = 1.
        check("A: stamina no-gear = 1 (class mod)", s.attribute(kSta) == 1);
        // intellect / spirit: no mods -> 0.
        check("A: intellect no-gear = 0", s.attribute(kInt) == 0);
        check("A: spirit no-gear = 0", s.attribute(kSpi) == 0);
        // derived armor: 0 base + 5 race = 5 (attribute layer only).
        check("A: armor attribute no-gear = 5 (race mod)", s.attribute(kArmor) == 5);
        check("A: crit no-gear = 0", s.attribute(kCrit) == 0);
        // Every catalog attribute is present in the snapshot.
        check("A: snapshot has all 7 attributes", s.attributes.size() == 7);
        // No gear -> no gear armor.
        check("A: gear_armor no-gear = 0", s.gear_armor == 0);
    }

    // --- B. Gear StatMods sum into the right attributes ---------------------
    {
        // A chest: +10 strength, +4 stamina. A ring-like item: +3 agility.
        const itm::ItemTemplate chest =
            make_item(1, {{itm::StatKey::kStrength, 10}, {itm::StatKey::kStamina, 4}}, 0);
        const itm::ItemTemplate trinket =
            make_item(2, {{itm::StatKey::kAgility, 3}}, 0);
        const std::vector<const itm::ItemTemplate*> loadout{&chest, &trinket};

        const auto s = aggregate_character_stats(cat, kRace, kClass, kLevel, loadout);
        // strength: 2 (class) + 10 (gear) = 12.
        check("B: strength = 2 + 10 gear = 12", s.attribute(kStr) == 12);
        // stamina: 1 (class) + 4 (gear) = 5.
        check("B: stamina = 1 + 4 gear = 5", s.attribute(kSta) == 5);
        // agility: 1 (race) + 3 (gear) = 4.
        check("B: agility = 1 + 3 gear = 4", s.attribute(kAgi) == 4);
        // intellect untouched (no gear mod).
        check("B: intellect untouched = 0", s.attribute(kInt) == 0);
    }

    // --- B2. Two items stacking on the SAME attribute -----------------------
    {
        const itm::ItemTemplate a = make_item(3, {{itm::StatKey::kStrength, 5}}, 0);
        const itm::ItemTemplate b = make_item(4, {{itm::StatKey::kStrength, 7}}, 0);
        const auto s = aggregate_character_stats(cat, kRace, kClass, kLevel, {&a, &b});
        // strength: 2 class + 5 + 7 = 14.
        check("B2: strength stacks 2 + 5 + 7 = 14", s.attribute(kStr) == 14);
    }

    // --- C. Gear armor sums into gear_armor, distinct from the attribute ----
    {
        const itm::ItemTemplate plate =
            make_item(5, {{itm::StatKey::kStamina, 2}}, 120);
        const itm::ItemTemplate boots = make_item(6, {}, 45);
        const auto s = aggregate_character_stats(cat, kRace, kClass, kLevel, {&plate, &boots});
        // gear_armor: 120 + 45 = 165.
        check("C: gear_armor = 120 + 45 = 165", s.gear_armor == 165);
        // The DERIVED armor ATTRIBUTE is still just the race mod (5) — gear armor did
        // NOT bleed into it (kept distinct for #785 / S5b).
        check("C: armor attribute stays 5 (gear armor NOT folded in)",
              s.attribute(kArmor) == 5);
        // stamina still gets the gear StatMod: 1 class + 2 gear = 3.
        check("C: stamina = 1 + 2 gear = 3 (mods still apply)", s.attribute(kSta) == 3);
    }

    // --- D. Unequip / replacement removes the contribution EXACTLY once ------
    {
        const itm::ItemTemplate old_chest =
            make_item(7, {{itm::StatKey::kStrength, 10}}, 100);
        const itm::ItemTemplate new_chest =
            make_item(8, {{itm::StatKey::kStrength, 4}}, 30);

        // Equipped with the old chest.
        const auto before = aggregate_character_stats(cat, kRace, kClass, kLevel, {&old_chest});
        check("D: with old chest strength = 2 + 10 = 12", before.attribute(kStr) == 12);
        check("D: with old chest gear_armor = 100", before.gear_armor == 100);

        // Replace old with new (re-aggregate the new loadout): the old +10 is gone
        // exactly once, the new +4 applies — no residue, no double-removal.
        const auto after = aggregate_character_stats(cat, kRace, kClass, kLevel, {&new_chest});
        check("D: after replace strength = 2 + 4 = 6 (old +10 removed once)",
              after.attribute(kStr) == 6);
        check("D: after replace gear_armor = 30 (old 100 removed once)",
              after.gear_armor == 30);

        // Fully unequip -> back to the no-gear baseline exactly.
        const auto bare = aggregate_character_stats(cat, kRace, kClass, kLevel, {});
        check("D: fully unequipped strength back to 2", bare.attribute(kStr) == 2);
        check("D: fully unequipped gear_armor back to 0", bare.gear_armor == 0);
    }

    // --- E. Cross-check straight against the EffectiveStats kernel -----------
    // The aggregator must equal the kernel's own effective() with the summed gear
    // StatMod as a flat delta — proving it never diverges from #694's math.
    {
        const itm::ItemTemplate chest =
            make_item(9, {{itm::StatKey::kStrength, 8}, {itm::StatKey::kAgility, 6}}, 0);
        const auto s = aggregate_character_stats(cat, kRace, kClass, kLevel, {&chest});

        EffectiveStats kernel(cat, kRace, kClass);  // base defaults to 0 (zero-base)
        AttributeDelta str_gear;
        str_gear.flat = 8;
        AttributeDelta agi_gear;
        agi_gear.flat = 6;
        AttributeDelta none;  // no gear mod for the derived armor
        check("E: strength matches kernel effective()",
              s.attribute(kStr) == kernel.effective(kStr, str_gear));
        check("E: agility matches kernel effective()",
              s.attribute(kAgi) == kernel.effective(kAgi, agi_gear));
        check("E: armor(derived) matches kernel effective() with no gear",
              s.attribute(kArmor) == kernel.effective(kArmor, none));
    }

    // --- F. Level is echoed and does NOT scale attributes -------------------
    {
        const auto lvl5 = aggregate_character_stats(cat, kRace, kClass, 5, {});
        const auto lvl60 = aggregate_character_stats(cat, kRace, kClass, 60, {});
        check("F: level echoed onto snapshot (5)", lvl5.level == 5);
        check("F: level echoed onto snapshot (60)", lvl60.level == 60);
        // Zero-base design: level does not (yet) change any attribute.
        check("F: strength identical across levels (no curve)",
              lvl5.attribute(kStr) == lvl60.attribute(kStr));
    }

    // --- G. Robustness: nullptr templates skipped; unknown ref reads 0 ------
    {
        const itm::ItemTemplate real = make_item(10, {{itm::StatKey::kStrength, 5}}, 20);
        const std::vector<const itm::ItemTemplate*> withNull{nullptr, &real, nullptr};
        const auto s = aggregate_character_stats(cat, kRace, kClass, kLevel, withNull);
        check("G: nullptr templates skipped (strength = 2 + 5 = 7)",
              s.attribute(kStr) == 7);
        check("G: nullptr templates skipped (gear_armor = 20)", s.gear_armor == 20);
        check("G: unknown ref reads 0", s.attribute("core:attribute.nope") == 0);
    }

    if (g_fail == 0) {
        std::printf("PASS: all effective-stat aggregator checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d effective-stat aggregator check(s) failed\n", g_fail);
    return 1;
}
