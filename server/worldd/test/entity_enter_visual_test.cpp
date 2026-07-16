// SPDX-License-Identifier: Apache-2.0
//
// worldd — EntityEnter appearance + equipped-visuals broadcast UNIT TEST
// (②/T1, issue #538; client-character-assembler design §2). Proves the additive
// EntityEnter visual-assembly fields the worldd encoder projects:
//
//   * a PLAYER entity (EntityIdentity::visual present) round-trips race, sex,
//     the §5.2 appearance record (contract ① T5) and the visible equipment set
//     — including a per-item dye choice carried as a NUMERIC dye id (the wire
//     shape the client resolves to a colour; §2 dye-resolution note);
//   * an NPC/creature entity (EntityIdentity::visual absent) carries NONE of the
//     new fields — appearance/equipment ABSENT (not zero-filled), race/sex 0 —
//     so the client keeps rendering it via the monolithic visual.model path.
//
// CLEAN-ROOM: written from the client-character-assembler design §2 + the
// world.fbs additive-field contract; no GPL source consulted (CONTRIBUTING.md).
//
// DB-free: exercises encode_entity_enter_payload() directly with a hand-built
// EntityIdentity + Unit — no TLS session, no MariaDB. Runs in the plain ctest.

#include "combat_unit.h"          // Player / placeholder_player_stats / Position
#include "movement_constants.h"
#include "movement_validation.h"  // Position
#include "world_generated.h"
#include "world_state.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace meridian::worldd;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
bool check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
    return ok;
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

}  // namespace

int main() {
    std::printf("worldd EntityEnter appearance + equipped-visuals test (②/T1, #538)\n");

    // ===== 1. PLAYER: appearance + equipment + dyes round-trip ===============
    // A player at the zone centre with a saved §5.2 appearance {v1,hair2,face3,
    // skin1}, race 3, and one visible equipped item (feet slot, template 42001)
    // dyed on the primary channel with a NUMERIC dye id (7). The numeric id is
    // what the wire carries (dye_id:uint32) — the enter-world builder sends the
    // dye vector EMPTY at M1 (no dye-application path yet, §2), so this hand-
    // seeded case is the sole exerciser of the populated dye shape.
    {
        EntityIdentity id;
        id.entity_guid = 42;
        id.type_id = 2;
        id.char_class = 2;
        id.name = "Aldric";

        CharacterVisual vis;
        vis.race = 3;
        vis.sex = 0;
        vis.appearance_version = 1;
        vis.hair = 2;
        vis.face = 3;
        vis.skin = 1;
        EquippedVisualRec boots;
        boots.slot = 8;               // EquipSlot::kFeet
        boots.item_template = 42001;  // IF-9 world id
        boots.dyes.push_back(DyeChoiceRec{/*channel=*/0, /*dye_id=*/7});
        vis.equipment.push_back(boots);
        id.visual = vis;

        Player unit(id.entity_guid, at(-320.0f, -320.0f),
                    placeholder_player_stats(id.char_class),
                    /*account_id=*/0, id.char_class, id.name);

        std::vector<std::uint8_t> buf = encode_entity_enter_payload(id, unit);

        fb::Verifier v(buf.data(), buf.size());
        check("1: player EntityEnter verifies", v.VerifyBuffer<mn::EntityEnter>(nullptr));
        const auto* e = fb::GetRoot<mn::EntityEnter>(buf.data());

        check("1: race round-trips", e->race() == 3);
        check("1: sex is 0 (male, M1)", e->sex() == 0);

        const auto* a = e->appearance();
        if (check("1: appearance present", a != nullptr)) {
            check("1: appearance.version", a->version() == 1);
            check("1: appearance.hair", a->hair() == 2);
            check("1: appearance.face", a->face() == 3);
            check("1: appearance.skin", a->skin() == 1);
        }

        const auto* eq = e->equipment();
        if (check("1: equipment present with 1 item", eq != nullptr && eq->size() == 1)) {
            const auto* v0 = eq->Get(0);
            check("1: equipment[0].slot", v0->slot() == 8);
            check("1: equipment[0].item_template", v0->item_template() == 42001u);
            const auto* dyes = v0->dyes();
            if (check("1: equipment[0] has 1 dye", dyes != nullptr && dyes->size() == 1)) {
                check("1: dye channel", dyes->Get(0)->channel() == 0);
                check("1: dye_id is the numeric id", dyes->Get(0)->dye_id() == 7u);
            }
        }
    }

    // ===== 2. NPC/creature: NO appearance/equipment (absent, not defaults) ====
    // A world entity relay (WorldEntityRec) never sets EntityIdentity::visual, so
    // its EntityEnter must omit the visual-assembly fields entirely: appearance
    // and equipment ABSENT, race/sex at the 0 default. This is the branch that
    // keeps NPC rendering on the monolithic visual.model path.
    {
        EntityIdentity id;
        id.entity_guid = 99;
        id.type_id = 500;
        id.name = "Sentry";
        // id.visual deliberately left nullopt (the NPC branch).

        Player unit(id.entity_guid, at(-320.0f, -320.0f), placeholder_player_stats(1),
                    /*account_id=*/0, /*char_class=*/1, id.name);

        std::vector<std::uint8_t> buf = encode_entity_enter_payload(id, unit);

        fb::Verifier v(buf.data(), buf.size());
        check("2: NPC EntityEnter verifies", v.VerifyBuffer<mn::EntityEnter>(nullptr));
        const auto* e = fb::GetRoot<mn::EntityEnter>(buf.data());

        check("2: appearance ABSENT for NPC", e->appearance() == nullptr);
        check("2: equipment ABSENT for NPC", e->equipment() == nullptr);
        check("2: race is 0 (unset) for NPC", e->race() == 0);
        check("2: sex is 0 (unset) for NPC", e->sex() == 0);
        // The pre-#538 vitals block still decodes (additive evolution).
        check("2: name still round-trips", e->name() && e->name()->str() == "Sentry");
    }

    // ===== 3. Replacement update reaches owner + observer ===================
    {
        WorldState world;
        std::vector<std::uint32_t> owner_items, observer_items, late_observer_items;
        auto sink = [](std::vector<std::uint32_t>& out) {
            return [&out](mn::Opcode op, const std::vector<std::uint8_t>& payload) {
                if (op != mn::Opcode::EQUIPMENT_VISUAL_UPDATE) return true;
                fb::Verifier v(payload.data(), payload.size());
                if (!v.VerifyBuffer<mn::EquipmentVisualUpdate>(nullptr)) return false;
                const auto* u = fb::GetRoot<mn::EquipmentVisualUpdate>(payload.data());
                out.clear();
                if (u->equipment()) for (const auto* row : *u->equipment())
                    out.push_back(row->item_template());
                return true;
            };
        };
        EntityIdentity owner;
        owner.entity_guid = 501; owner.char_class = 1; owner.name = "Owner";
        owner.visual = CharacterVisual{};
        EntityIdentity observer;
        observer.entity_guid = 502; observer.char_class = 1; observer.name = "Observer";
        observer.visual = CharacterVisual{};
        world.enter(owner, at(-320.0f, -320.0f), sink(owner_items));
        world.enter(observer, at(-320.0f, -320.0f), sink(observer_items));
        EquippedVisualRec replacement;
        replacement.slot = 13; replacement.item_template = 777001;
        const auto recipients = world.update_equipment_visuals(501, {replacement});
        check("3: visual replacement reaches owner + observer", recipients == 2);
        check("3: owner receives full replacement", owner_items == std::vector<std::uint32_t>{777001});
        check("3: observer receives full replacement", observer_items == std::vector<std::uint32_t>{777001});

        // A session entering AoI after the mutation must receive the stored full
        // replacement in EntityEnter (the reconnect/late-observer consistency seam).
        EntityIdentity late;
        late.entity_guid = 503; late.char_class = 1; late.name = "Late observer";
        late.visual = CharacterVisual{};
        world.enter(late, at(-320.0f, -320.0f),
                    [&late_observer_items](mn::Opcode op,
                                           const std::vector<std::uint8_t>& payload) {
            if (op != mn::Opcode::ENTITY_ENTER) return true;
            fb::Verifier v(payload.data(), payload.size());
            if (!v.VerifyBuffer<mn::EntityEnter>(nullptr)) return false;
            const auto* e = fb::GetRoot<mn::EntityEnter>(payload.data());
            if (e->entity_guid() != 501) return true;
            late_observer_items.clear();
            if (e->equipment()) for (const auto* row : *e->equipment())
                late_observer_items.push_back(row->item_template());
            return true;
        });
        check("3: late observer EntityEnter carries stored replacement",
              late_observer_items == std::vector<std::uint32_t>{777001});
    }

    if (g_fail == 0) {
        std::printf("PASS: EntityEnter + live equipment visual broadcast\n");
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
