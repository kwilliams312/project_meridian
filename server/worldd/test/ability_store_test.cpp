// SPDX-License-Identifier: Apache-2.0
//
// worldd — compiled ability/effect data model + store UNIT TEST (issue #343,
// CMB-01 foundation).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5/§3.3, client SAD §2.4 (the
// datastore lookup contract), and the IF-4 world DDL schema/sql/world/30_ability.sql
// + schema/content/ability.schema.yaml. No GPL / emulator source consulted
// (CONTRIBUTING).
//
// PURE / DB-FREE: drives AbilityStore + placeholder_ability_set() directly over
// in-memory data — no DB, no socket, no I/O — so it runs in the PLAIN server ctest
// (no MariaDB service). This is the "unit tests for load + lookup + the placeholder
// set's shape" ask (#343).
//
// What it proves:
//   A. Load: from_abilities() builds an O(1)-keyed store; find() returns the row
//      for a known id, nullptr for an unknown id (the §2.4 "never crash" miss).
//   B. Read-only lookup: contains()/size()/empty() reflect the loaded set; a found
//      Ability's fields round-trip exactly (the model carries the data faithfully).
//   C. Duplicate ids: a later row with a duplicate id is dropped first-wins and
//      reported via duplicate_id_out (a bad pack degrades, never crashes boot).
//   D. Placeholder set SHAPE: exactly the four expected abilities (melee strike,
//      nuke, heal, DoT), each with the distinguishing shape the resolver stories
//      exercise (instant/melee/free damage; cast/ranged/mana damage; cast/friendly/
//      mana heal; instant/enemy/mana periodic-aura DoT).
//   E. Effect model: the DoT's aura effect carries a periodic damage tick; the
//      damage/heal effects carry an amount range.

#include "ability_store.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// A minimal well-formed ability row for load/lookup tests.
Ability make_ability(AbilityId id, const std::string& name = "test") {
    Ability a;
    a.id = id;
    a.name = name;
    a.school = School::kFire;
    a.target = TargetKind::kEnemy;
    a.range_m = 30.0f;
    a.cast_time_ms = 1500;
    a.resource_type = AbilityResourceType::kMana;
    a.resource_amount = 20;
    AbilityEffect e;
    e.kind = EffectKind::kDamage;
    e.amount_min = 10;
    e.amount_max = 15;
    a.effects.push_back(e);
    return a;
}

// Find the single placeholder ability with `id` in the set (nullptr if absent).
const Ability* find_in(const std::vector<Ability>& set, AbilityId id) {
    for (const Ability& a : set) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

}  // namespace

int main() {
    std::printf("worldd ability/effect data model + store unit test\n");

    // A. Load + O(1) lookup: known id hits, unknown id misses (nullptr).
    {
        AbilityStore store = AbilityStore::from_abilities(
            {make_ability(100, "alpha"), make_ability(200, "beta")});
        check("A size 2 after load", store.size() == 2);
        check("A not empty", !store.empty());
        const Ability* a = store.find(100);
        check("A find(100) hits", a != nullptr);
        check("A find(100) is the right row", a != nullptr && a->name == "alpha");
        check("A find(200) hits", store.find(200) != nullptr);
        check("A contains(200)", store.contains(200));
        check("A find(999) misses -> nullptr (never crash)", store.find(999) == nullptr);
        check("A !contains(999)", !store.contains(999));
    }

    // A'. Empty store is valid and misses everything.
    {
        AbilityStore empty = AbilityStore::from_abilities({});
        check("A' empty store size 0", empty.size() == 0);
        check("A' empty store .empty()", empty.empty());
        check("A' empty store find misses", empty.find(1) == nullptr);
    }

    // B. Field round-trip: the model carries the row faithfully.
    {
        Ability src = make_ability(42, "roundtrip");
        src.range_m = 12.5f;
        src.triggers_gcd = false;
        src.cooldown_ms = 6000;
        AbilityStore store = AbilityStore::from_abilities({src});
        const Ability* a = store.find(42);
        check("B round-trip found", a != nullptr);
        if (a) {
            check("B id", a->id == 42);
            check("B name", a->name == "roundtrip");
            check("B school", a->school == School::kFire);
            check("B target", a->target == TargetKind::kEnemy);
            check("B range_m", a->range_m == 12.5f);
            check("B cast_time_ms", a->cast_time_ms == 1500);
            check("B triggers_gcd false", a->triggers_gcd == false);
            check("B cooldown_ms", a->cooldown_ms == 6000);
            check("B resource_type mana", a->resource_type == AbilityResourceType::kMana);
            check("B resource_amount", a->resource_amount == 20);
            check("B one effect", a->effects.size() == 1);
            check("B effect kind damage",
                  a->effects.size() == 1 && a->effects[0].kind == EffectKind::kDamage);
            check("B effect amount range",
                  a->effects.size() == 1 && a->effects[0].amount_min == 10 &&
                      a->effects[0].amount_max == 15);
        }
    }

    // C. Duplicate id -> first-wins drop + reported (bad pack degrades, no crash).
    {
        AbilityId dup = 0;
        AbilityStore store = AbilityStore::from_abilities(
            {make_ability(7, "first"), make_ability(7, "second"), make_ability(8, "other")},
            &dup);
        check("C duplicate reported", dup == 7);
        check("C only 2 distinct ids loaded", store.size() == 2);
        const Ability* a = store.find(7);
        check("C first row wins", a != nullptr && a->name == "first");
        check("C non-dup id present", store.contains(8));
    }

    // D. Placeholder set SHAPE — exactly the four expected abilities, each with the
    //    distinguishing shape the resolver stories exercise.
    {
        std::vector<Ability> set = placeholder_ability_set();
        check("D placeholder set has exactly 4 abilities", set.size() == 4);

        // Melee strike: instant, melee range, physical, direct damage, free.
        const Ability* melee = find_in(set, kPlaceholderMeleeStrikeId);
        check("D melee present", melee != nullptr);
        if (melee) {
            check("D melee instant (cast_time 0)", melee->cast_time_ms == 0);
            check("D melee is melee range (5m)", melee->range_m == 5.0f);
            check("D melee physical school", melee->school == School::kPhysical);
            check("D melee targets enemy", melee->target == TargetKind::kEnemy);
            check("D melee has a damage effect",
                  !melee->effects.empty() && melee->effects[0].kind == EffectKind::kDamage);
        }

        // Nuke: cast time, ranged, fire, mana cost, direct damage.
        const Ability* nuke = find_in(set, kPlaceholderNukeId);
        check("D nuke present", nuke != nullptr);
        if (nuke) {
            check("D nuke has cast time", nuke->cast_time_ms > 0);
            check("D nuke is ranged (>5m)", nuke->range_m > 5.0f);
            check("D nuke fire school", nuke->school == School::kFire);
            check("D nuke costs mana",
                  nuke->resource_type == AbilityResourceType::kMana && nuke->resource_amount > 0);
            check("D nuke has a damage effect",
                  !nuke->effects.empty() && nuke->effects[0].kind == EffectKind::kDamage);
        }

        // Heal: cast time, friendly target, holy, mana cost, heal effect.
        const Ability* heal = find_in(set, kPlaceholderHealId);
        check("D heal present", heal != nullptr);
        if (heal) {
            check("D heal has cast time", heal->cast_time_ms > 0);
            check("D heal targets friendly", heal->target == TargetKind::kFriendly);
            check("D heal holy school", heal->school == School::kHoly);
            check("D heal costs mana", heal->resource_type == AbilityResourceType::kMana);
            check("D heal has a heal effect",
                  !heal->effects.empty() && heal->effects[0].kind == EffectKind::kHeal);
        }

        // DoT: instant apply, enemy, shadow, mana cost, aura effect w/ periodic dmg.
        const Ability* dot = find_in(set, kPlaceholderDotId);
        check("D dot present", dot != nullptr);
        if (dot) {
            check("D dot targets enemy", dot->target == TargetKind::kEnemy);
            check("D dot shadow school", dot->school == School::kShadow);
            check("D dot has an aura effect",
                  !dot->effects.empty() && dot->effects[0].kind == EffectKind::kAura);
            if (!dot->effects.empty() && dot->effects[0].kind == EffectKind::kAura) {
                const AbilityEffect& aura = dot->effects[0];
                // E. aura carries a periodic damage tick with a duration + interval.
                check("E dot aura has duration", aura.duration_ms > 0);
                check("E dot aura periodic is damage",
                      aura.periodic_kind == PeriodicKind::kDamage);
                check("E dot aura periodic ticks", aura.periodic_tick_ms > 0);
                check("E dot aura periodic has magnitude",
                      aura.periodic_amount_max > 0);
            }
        }
    }

    // D'. The placeholder set loads cleanly into a store (no duplicate ids), and
    //     every placeholder id resolves through the O(1) lookup.
    {
        AbilityId dup = 0xFFFFFFFFu;
        AbilityStore store = AbilityStore::from_abilities(placeholder_ability_set(), &dup);
        check("D' placeholder set has no duplicate ids", dup == 0xFFFFFFFFu);
        check("D' placeholder store size 4", store.size() == 4);
        check("D' melee resolves", store.contains(kPlaceholderMeleeStrikeId));
        check("D' nuke resolves", store.contains(kPlaceholderNukeId));
        check("D' heal resolves", store.contains(kPlaceholderHealId));
        check("D' dot resolves", store.contains(kPlaceholderDotId));

        // load_placeholder_ability_store() convenience == the same set.
        AbilityStore conv = load_placeholder_ability_store();
        check("D' convenience loader size 4", conv.size() == 4);
        check("D' convenience loader melee resolves",
              conv.contains(kPlaceholderMeleeStrikeId));
    }

    // F. Placeholder id band is the obviously-synthetic high band (won't collide
    //    with a real IF-9 pack band).
    {
        check("F placeholder band is 0xF0000000", kPlaceholderIdBand == 0xF0000000u);
        check("F melee id in band", kPlaceholderMeleeStrikeId == 0xF0000000u + 1);
    }

    // G. Enum name helpers (diagnostics).
    {
        check("G school_name fire", std::string(school_name(School::kFire)) == "fire");
        check("G target_kind_name friendly",
              std::string(target_kind_name(TargetKind::kFriendly)) == "friendly");
        check("G resource_type_name none",
              std::string(resource_type_name(AbilityResourceType::kNone)) == "none");
        check("G effect_kind_name aura",
              std::string(effect_kind_name(EffectKind::kAura)) == "aura");
    }

    if (g_fail == 0) {
        std::printf("PASS: all ability data-model + store checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d ability data-model + store check(s) failed\n", g_fail);
    return 1;
}
