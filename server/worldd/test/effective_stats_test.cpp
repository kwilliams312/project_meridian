// SPDX-License-Identifier: Apache-2.0
//
// worldd — EFFECTIVE-STAT framework UNIT test (SP2.4 #694, epic #690).
//
// PURE / DB-FREE: drives AttributeCatalog + EffectiveStats + AuraContainer over
// in-memory data — no DB, no socket, no clock — so it runs in the PLAIN server
// ctest (no MariaDB). The DB-backed proof (class/race mods loaded from a real
// MariaDB) is the separate effective_stats_db_it (test.sh --db).
//
// What it proves:
//   A. Catalog: attribute defs (primary/derived) + per-class/per-race mods register
//      and read back; unknown refs are safe (nullptr / 0).
//   B. Static value: effective-at-rest = base + class_mod + race_mod (both layers).
//   C. Effective (flat): a live buff FLAT delta folds on top of base + pack mods.
//   D. Effective (percent): a percent delta scales the flat total (round half away),
//      including a debuff (negative flat / percent).
//   E. DERIVED attribute: crit/armor (no StatKey) compute the same way — the #694
//      point (a derived buff had no consumer before this framework).
//   F. Unification with the SP2.3 aura ledger: reading effective() straight off an
//      AuraContainer (a kBuff flat on a primary + a kBuff percent on a derived)
//      matches, and EXPIRY rolls the contribution back to the static value.

#include "ability_store.h"
#include "aura_container.h"
#include "combat_resolver.h"
#include "combat_unit.h"
#include "effective_stats.h"
#include "movement_validation.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace meridian::worldd;

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
const std::string kArmor = "core:attribute.armor";  // derived
const std::string kCrit = "core:attribute.crit";    // derived

AttributeCatalog seed_catalog() {
    AttributeCatalog c;
    c.add_attribute({kStr, "Strength", AttributeKind::kPrimary, 1});
    c.add_attribute({kAgi, "Agility", AttributeKind::kPrimary, 2});
    c.add_attribute({kSta, "Stamina", AttributeKind::kPrimary, 3});
    c.add_attribute({kArmor, "Armor", AttributeKind::kDerived, 4});
    c.add_attribute({kCrit, "Critical Strike", AttributeKind::kDerived, 5});
    // Vanguard (class roster 1): strength +2, stamina +1 (the seed pack tuning).
    c.add_class_mod(1, kStr, 2);
    c.add_class_mod(1, kSta, 1);
    // A tuned race (race roster 2): agility +1 and a DERIVED armor +5 (proves race
    // mods reach derived attributes too).
    c.add_race_mod(2, kAgi, 1);
    c.add_race_mod(2, kArmor, 5);
    return c;
}

// A minimal host Unit for the AuraContainer integration section.
Unit make_unit() {
    UnitStats st;
    st.level = 10;
    st.max_health = 1000;
    return Unit{1, ObjectType::kPlayer, Position{0, 0, 0}, st};
}

}  // namespace

int main() {
    std::printf("effective-stats framework unit test (#694)\n");

    const AttributeCatalog cat = seed_catalog();

    // --- A. Catalog ---------------------------------------------------------
    {
        check("A: catalog has 5 attributes", cat.attribute_count() == 5);
        check("A: 3 primary + 2 derived", cat.primary_count() == 3 && cat.derived_count() == 2);
        check("A: strength is primary", cat.is_primary(kStr));
        check("A: armor is NOT primary (derived)", !cat.is_primary(kArmor));
        check("A: unknown ref is not primary", !cat.is_primary("core:attribute.nope"));
        check("A: find(strength) resolves name", cat.find(kStr) != nullptr &&
              cat.find(kStr)->name == "Strength");
        check("A: find(unknown) is nullptr", cat.find("core:attribute.nope") == nullptr);
        check("A: class 1 strength mod +2", cat.class_mod(1, kStr) == 2);
        check("A: class 1 stamina mod +1", cat.class_mod(1, kSta) == 1);
        check("A: class 1 agility mod 0 (untuned)", cat.class_mod(1, kAgi) == 0);
        check("A: unknown class mod 0", cat.class_mod(9, kStr) == 0);
        check("A: race 2 agility mod +1", cat.race_mod(2, kAgi) == 1);
        check("A: race 2 armor mod +5 (derived)", cat.race_mod(2, kArmor) == 5);
        check("A: race 1 has no mods (chibi zero)", cat.race_mod(1, kStr) == 0);
    }

    // --- B. Static value = base + class + race ------------------------------
    // A Vanguard (class 1) of the tuned race (race 2), base strength 10 / agility 8.
    {
        EffectiveStats es(cat, /*race=*/2, /*class=*/1);
        es.set_base(kStr, 10);
        es.set_base(kAgi, 8);
        es.set_base(kSta, 5);
        // strength: 10 base + 2 class + 0 race = 12.
        check("B: strength static 10+2 = 12", es.static_value(kStr) == 12);
        // agility: 8 base + 0 class + 1 race = 9.
        check("B: agility static 8+1 = 9", es.static_value(kAgi) == 9);
        // stamina: 5 base + 1 class + 0 race = 6.
        check("B: stamina static 5+1 = 6", es.static_value(kSta) == 6);
        // a base not set is 0; armor derived = 0 base + 0 class + 5 race = 5.
        check("B: armor static 0+5(race) = 5", es.static_value(kArmor) == 5);
        check("B: base(strength) = 10", es.base(kStr) == 10);
        check("B: base(unset) = 0", es.base(kCrit) == 0);
    }

    // --- C. Effective with a FLAT buff --------------------------------------
    {
        EffectiveStats es(cat, 2, 1);
        es.set_base(kStr, 10);
        // A +15 flat buff (no percent).
        AttributeDelta d;
        d.flat = 15;
        // 10 base + 2 class + 0 race + 15 buff = 27.
        check("C: strength effective (flat +15) = 27", es.effective(kStr, d) == 27);
        const EffectiveStat b = es.breakdown(kStr, d);
        check("C: breakdown base 10", b.base == 10);
        check("C: breakdown flat_mods 2+0+15 = 17", b.flat_mods == 17);
        check("C: breakdown percent 0", b.percent == 0);
        check("C: breakdown value 27", b.value == 27);
    }

    // --- D. Effective with PERCENT (round half away) + a debuff -------------
    {
        EffectiveStats es(cat, 2, 1);
        es.set_base(kStr, 10);
        // static strength = 12. +25.00%% (percent = 2500): 12 * 1.25 = 15.0 -> 15.
        AttributeDelta up;
        up.percent = 2500;
        check("D: 12 * 1.25 = 15", es.effective(kStr, up) == 15);
        // +10.00%% (percent = 1000): 12 * 1.10 = 13.2 -> 13 (rounds down).
        AttributeDelta up2;
        up2.percent = 1000;
        check("D: 12 * 1.10 = 13.2 -> 13", es.effective(kStr, up2) == 13);
        // +5.00%% (500) on static 5 (armor): 5 * 1.05 = 5.25 -> 5.
        EffectiveStats esA(cat, 2, 1);
        AttributeDelta ap;
        ap.percent = 500;
        check("D: armor 5 * 1.05 = 5.25 -> 5", esA.effective(kArmor, ap) == 5);
        // Round half AWAY from zero: static 10 * 1.05 = 10.5 -> 11.
        EffectiveStats esB(cat, /*race=*/1, /*class=*/9);  // no mods -> static == base
        esB.set_base(kCrit, 10);
        AttributeDelta half;
        half.percent = 500;
        check("D: 10 * 1.05 = 10.5 -> 11 (half away)", esB.effective(kCrit, half) == 11);
        // A DEBUFF: flat -4 and -50.00%% on static 12: (12-4)=8 * 0.50 = 4.
        AttributeDelta down;
        down.flat = -4;
        down.percent = -5000;
        check("D: debuff (12-4)*0.5 = 4", es.effective(kStr, down) == 4);
        // Negative flat_total, positive percent still rounds away from zero:
        // static 0 crit, flat -10, +5%%: -10 * 1.05 = -10.5 -> -11.
        EffectiveStats esC(cat, 1, 9);
        AttributeDelta neg;
        neg.flat = -10;
        neg.percent = 500;
        check("D: -10 * 1.05 = -10.5 -> -11 (half away, negative)",
              esC.effective(kCrit, neg) == -11);
    }

    // --- E. DERIVED attribute computes through the framework ----------------
    {
        EffectiveStats es(cat, 2, 1);
        // Armor derived: base 0 + race 5. A +30 flat buff + 20.00%% -> (0+5+30)*1.20
        // = 42.0 -> 42.
        AttributeDelta d;
        d.flat = 30;
        d.percent = 2000;
        check("E: armor (5+30)*1.20 = 42", es.effective(kArmor, d) == 42);
        const EffectiveStat b = es.breakdown(kArmor, d);
        check("E: armor breakdown flat_mods 5(race)+30 = 35", b.flat_mods == 35);
        check("E: armor breakdown value 42", b.value == 42);
    }

    // --- F. Unification with the SP2.3 AuraContainer ledger -----------------
    // The framework reads the live buff/debuff layer STRAIGHT off the container's
    // interim ledger (attribute_delta) — the #694 consumer of #693's seam. A kBuff
    // on a PRIMARY (flat, folds into StatKey too) and a kBuff PERCENT on a DERIVED
    // attribute (armor — no StatKey, ledger-only) both resolve through effective(),
    // and expiry rolls both back.
    {
        Unit host = make_unit();
        AuraContainer auras{host};
        EffectiveStats es(cat, 2, 1);  // Vanguard / tuned race
        es.set_base(kStr, 10);         // static strength = 12
        // static armor = 5 (race mod).

        // Before any aura: effective == static.
        check("F: strength effective == static (no auras) = 12", es.effective(kStr, auras) == 12);
        check("F: armor effective == static (no auras) = 5", es.effective(kArmor, auras) == 5);

        // Build a two-effect ability: [0] a FLAT +8 strength buff (primary),
        // [1] a +40.00%% armor buff (derived, percent-only).
        Ability buff;
        buff.id = 7000;
        AbilityEffect e0;
        e0.kind = EffectKind::kBuff;
        e0.duration_ms = 10'000;
        e0.attribute = kStr;
        e0.attribute_amount = 8;
        e0.attribute_modifier = AttributeModifier::kFlat;
        buff.effects.push_back(e0);
        AbilityEffect e1;
        e1.kind = EffectKind::kBuff;
        e1.duration_ms = 10'000;
        e1.attribute = kArmor;
        e1.attribute_amount = 4000;  // +40.00%%
        e1.attribute_modifier = AttributeModifier::kPercent;
        buff.effects.push_back(e1);

        const std::size_t applied = auras.apply_ability_effects(buff, /*caster=*/99);
        check("F: two buff effects applied", applied == 2);

        // strength: static 12 + 8 flat = 20 (percent 0).
        check("F: strength effective with flat buff = 20", es.effective(kStr, auras) == 20);
        // The primary flat ALSO mirrors onto the StatKey layer (SP2.3), so both
        // views stay coherent.
        check("F: StatKey strength delta mirrors +8", auras.stat_delta(StatKey::kStrength) == 8);
        // armor: static 5, +40.00%% -> 5 * 1.40 = 7.0 -> 7.
        check("F: armor effective with 40%% buff = 7", es.effective(kArmor, auras) == 7);

        // Expire both auras and confirm the effective values roll back to static.
        CombatRng rng{0x1234u};
        auras.tick(10'000, rng);
        check("F: both auras expired", auras.empty());
        check("F: strength effective back to static 12", es.effective(kStr, auras) == 12);
        check("F: armor effective back to static 5", es.effective(kArmor, auras) == 5);
        check("F: StatKey strength delta back to 0", auras.stat_delta(StatKey::kStrength) == 0);
    }

    // --- G. Diagnostics -----------------------------------------------------
    {
        check("G: kind name primary", std::string(attribute_kind_name(AttributeKind::kPrimary)) == "primary");
        check("G: kind name derived", std::string(attribute_kind_name(AttributeKind::kDerived)) == "derived");
    }

    if (g_fail == 0) {
        std::printf("PASS: all effective-stat framework checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d effective-stat framework check(s) failed\n", g_fail);
    return 1;
}
