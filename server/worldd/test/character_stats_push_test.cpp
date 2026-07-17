// SPDX-License-Identifier: Apache-2.0
//
// worldd — CHARACTER_STATS wire + AoI-leak-guard UNIT test (SP2.5 #897; epic #866
// S5b; split from #871 per the 2026-07-17 scout).
//
// DB-FREE / socket-FREE / client-FREE: proves the two things S5b's private owner
// stat-push contract rests on, in isolation, so it always runs in the plain server
// ctest (no MariaDB):
//
//   1. WIRE ROUND-TRIP — the #896 aggregator's effective-stat snapshot (computed live
//      from class/race attribute_mods + level + EQUIPPED-GEAR StatMods/armor) encodes
//      through encode_character_stats() into a CHARACTER_STATS (0x0022) table and
//      decodes back to the SAME level, per-attribute effective values, and gear_armor.
//      The gear contribution is proven present (a gear StatMod raises the matching
//      primary; item armor lands in gear_armor, NOT the derived armor attribute), and
//      the entry order is DETERMINISTIC (sorted by ref — the golden-stability property
//      the conformance corpus relies on).
//
//   2. ⛔ THE SECURITY INVARIANT (ratified) — the AoI-BROADCAST encoders keep their
//      `attrs` vector EMPTY. encode_entity_enter_payload / encode_entity_update_payload
//      are sent once per OBSERVER (send_enter_entity), so a stat sheet on them would
//      leak every player's build to everyone in range. This test decodes both and
//      asserts attrs is absent/empty even for a fully-statted character — the sheet
//      rides ONLY the private CHARACTER_STATS push, never the broadcast.
//
// CLEAN-ROOM: written from the #896 aggregator output model, the world.fbs
// CharacterStats / EntityEnter / EntityUpdate contracts, and the worldd public headers
// only. No GPL/AGPL/emulator source consulted (CONTRIBUTING.md).

#include "character_stats_wire.h"
#include "combat_unit.h"          // Player / placeholder_player_stats / Position
#include "effective_stats.h"
#include "effective_stats_aggregator.h"
#include "item_template.h"
#include "movement_constants.h"
#include "world_state.h"          // encode_entity_enter_payload / encode_entity_update_payload

#include "world_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace itm = meridian::items;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Seed attribute vocabulary refs (SP1's meridian/attribute@1) — mirrors the #896
// aggregator unit test so the two reason about the same numbers.
const std::string kStr = "core:attribute.strength";
const std::string kAgi = "core:attribute.agility";
const std::string kSta = "core:attribute.stamina";
const std::string kInt = "core:attribute.intellect";
const std::string kSpi = "core:attribute.spirit";
const std::string kArmor = "core:attribute.armor";  // derived

// Vanguard (class roster 1): strength +2, stamina +1. A tuned race (roster 2):
// agility +1 and a DERIVED armor +5.
AttributeCatalog seed_catalog() {
    AttributeCatalog c;
    c.add_attribute({kStr, "Strength", AttributeKind::kPrimary, 1});
    c.add_attribute({kAgi, "Agility", AttributeKind::kPrimary, 2});
    c.add_attribute({kSta, "Stamina", AttributeKind::kPrimary, 3});
    c.add_attribute({kInt, "Intellect", AttributeKind::kPrimary, 4});
    c.add_attribute({kSpi, "Spirit", AttributeKind::kPrimary, 5});
    c.add_attribute({kArmor, "Armor", AttributeKind::kDerived, 6});
    c.add_class_mod(1, kStr, 2);
    c.add_class_mod(1, kSta, 1);
    c.add_race_mod(2, kAgi, 1);
    c.add_race_mod(2, kArmor, 5);
    return c;
}

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

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

// Look up a decoded CharacterStats attribute by ref (linear — the set is tiny).
std::int32_t wire_attr(const mn::CharacterStats* m, const std::string& ref) {
    if (m->attributes() == nullptr) return 0;
    for (const auto* e : *m->attributes()) {
        if (e->ref() != nullptr && e->ref()->str() == ref) return e->value();
    }
    return 0;
}

}  // namespace

int main() {
    std::printf("worldd CHARACTER_STATS wire + AoI-leak-guard unit test (#897)\n");

    const AttributeCatalog cat = seed_catalog();
    const std::uint8_t kRace = 2, kClass = 1;
    const std::uint16_t kLevel = 12;

    // ===== 1. WIRE ROUND-TRIP: aggregator -> CHARACTER_STATS -> decode ==========
    {
        // A chest: +10 strength, +4 stamina, 120 armor. Boots: 45 armor.
        const itm::ItemTemplate chest =
            make_item(1, {{itm::StatKey::kStrength, 10}, {itm::StatKey::kStamina, 4}}, 120);
        const itm::ItemTemplate boots = make_item(2, {}, 45);
        const std::vector<const itm::ItemTemplate*> loadout{&chest, &boots};

        const AggregatedCharacterStats stats =
            aggregate_character_stats(cat, kRace, kClass, kLevel, loadout);
        const std::vector<std::uint8_t> buf = encode_character_stats(stats);

        fb::Verifier v(buf.data(), buf.size());
        check("1: CharacterStats verifies", v.VerifyBuffer<mn::CharacterStats>(nullptr));
        const auto* m = fb::GetRoot<mn::CharacterStats>(buf.data());

        check("1: level round-trips (12)", m->level() == 12);
        // gear_armor = 120 + 45 = 165, distinct from the derived armor attribute.
        check("1: gear_armor = 120 + 45 = 165", m->gear_armor() == 165);
        // strength: 2 class + 10 gear = 12.
        check("1: strength = class 2 + gear 10 = 12", wire_attr(m, kStr) == 12);
        // stamina: 1 class + 4 gear = 5.
        check("1: stamina = class 1 + gear 4 = 5", wire_attr(m, kSta) == 5);
        // agility: 1 race, no gear = 1.
        check("1: agility = race 1 = 1", wire_attr(m, kAgi) == 1);
        // derived armor attribute = race 5 ONLY (gear armor did NOT fold in).
        check("1: armor attribute = race 5 (gear armor NOT folded in)",
              wire_attr(m, kArmor) == 5);
        // Every catalog attribute present on the wire.
        check("1: all 6 attributes on the wire",
              m->attributes() != nullptr && m->attributes()->size() == 6);

        // Order is DETERMINISTIC (sorted by ref) — the conformance-golden stability
        // property. agility < armor < intellect < spirit < stamina < strength.
        const auto* entries = m->attributes();
        bool sorted = true;
        for (fb::uoffset_t i = 1; entries != nullptr && i < entries->size(); ++i) {
            if (!(entries->Get(i - 1)->ref()->str() < entries->Get(i)->ref()->str()))
                sorted = false;
        }
        check("1: attribute entries are sorted by ref (deterministic order)", sorted);
    }

    // ===== 1b. Zero-gear sheet still round-trips (base class/race stats) ========
    {
        const AggregatedCharacterStats stats =
            aggregate_character_stats(cat, kRace, kClass, /*level=*/1, {});
        const std::vector<std::uint8_t> buf = encode_character_stats(stats);
        fb::Verifier v(buf.data(), buf.size());
        check("1b: gearless CharacterStats verifies",
              v.VerifyBuffer<mn::CharacterStats>(nullptr));
        const auto* m = fb::GetRoot<mn::CharacterStats>(buf.data());
        check("1b: level 1", m->level() == 1);
        check("1b: gear_armor 0 (no gear)", m->gear_armor() == 0);
        check("1b: strength = class 2 (no gear)", wire_attr(m, kStr) == 2);
    }

    // ===== 2. ⛔ SECURITY INVARIANT: AoI-broadcast encoders carry NO attrs ======
    // A fully-statted player: even so, the observer-broadcast EntityEnter/EntityUpdate
    // MUST NOT carry the stat sheet. The sheet is private (CHARACTER_STATS only).
    {
        EntityIdentity id;
        id.entity_guid = 7;
        id.type_id = 2;
        id.char_class = kClass;
        id.name = "Statful";
        Player unit(id.entity_guid, at(-320.0f, -320.0f),
                    placeholder_player_stats(id.char_class), /*account_id=*/0,
                    id.char_class, id.name);

        const std::vector<std::uint8_t> enter = encode_entity_enter_payload(id, unit);
        fb::Verifier ve(enter.data(), enter.size());
        check("2: EntityEnter verifies", ve.VerifyBuffer<mn::EntityEnter>(nullptr));
        const auto* e = fb::GetRoot<mn::EntityEnter>(enter.data());
        // attrs must be empty (or absent) — nothing rides the AoI broadcast.
        check("2: EntityEnter.attrs is empty (no stat leak on AoI broadcast)",
              e->attrs() == nullptr || e->attrs()->size() == 0);

        const std::vector<std::uint8_t> update =
            encode_entity_update_payload(id.entity_guid, at(-319.0f, -320.0f));
        fb::Verifier vu(update.data(), update.size());
        check("2: EntityUpdate verifies", vu.VerifyBuffer<mn::EntityUpdate>(nullptr));
        const auto* u = fb::GetRoot<mn::EntityUpdate>(update.data());
        check("2: EntityUpdate.attrs is empty (no stat leak on AoI delta)",
              u->attrs() == nullptr || u->attrs()->size() == 0);
    }

    if (g_fail == 0) {
        std::printf("PASS: all CHARACTER_STATS wire + leak-guard checks passed\n");
        return 0;
    }
    std::printf("FAIL: %d CHARACTER_STATS check(s) failed\n", g_fail);
    return 1;
}
