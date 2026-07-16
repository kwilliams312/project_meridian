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
//   * a MODEL-ONLY entity (EntityIdentity::visual absent) carries NONE of the
//     new fields — appearance/equipment ABSENT (not zero-filled), race/sex 0 —
//     so the client keeps rendering it via the monolithic visual.model path;
//   * an APPEARANCE-CARRYING NPC (npc@2, contract ①/§7, #821: char_class 0, visual
//     present, no equipment) round-trips race + the appearance record just like a
//     player — proving the encoder is gated on `visual` being set, NOT on the entity
//     being a player. This is the wire proof of the story-1 mechanism.
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

    // ===== 2. MODEL-ONLY entity: NO appearance/equipment (absent, not defaults) ==
    // A model-only relay (WorldEntityRec with EntityIdentity::visual unset — every M1
    // NPC/creature) must omit the visual-assembly fields entirely: appearance and
    // equipment ABSENT, race/sex at the 0 default. This is the branch that keeps
    // model-only rendering on the monolithic visual.model path.
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

    // ===== 3. APPEARANCE-CARRYING NPC: same block a player carries (npc@2) =======
    // The story-1 mechanism (contract ①/§7, #821): an NPC that carries an
    // appearance_catalog projects EntityIdentity::visual (install_spawns copies the DB
    // projection). It is NOT a player — char_class 0, no equipment — yet its
    // EntityEnter MUST carry race + the appearance record, because the encoder emits
    // the block whenever `visual` is set, not when the entity is a player. This is the
    // DB-free wire proof that an NPC CAN carry an appearance (story 2 turns it on).
    {
        EntityIdentity id;
        id.entity_guid = 77;
        id.type_id = 4200;   // an npc_template id, not a player
        id.char_class = 0;   // NPCs carry no class (install_spawns sets 0)
        id.name = "Tansy Dewhollow";

        CharacterVisual vis;
        vis.race = 1;   // a chibi race roster id (the wire `race`)
        vis.sex = 0;    // male
        vis.appearance_version = 1;
        vis.hair = 1;
        vis.face = 1;
        vis.skin = 1;
        // equipment left empty — worn_items is not projected at M1.
        id.visual = vis;

        Player unit(id.entity_guid, at(-320.0f, -320.0f), placeholder_player_stats(0),
                    /*account_id=*/0, /*char_class=*/0, id.name);

        std::vector<std::uint8_t> buf = encode_entity_enter_payload(id, unit);

        fb::Verifier v(buf.data(), buf.size());
        check("3: appearance-NPC EntityEnter verifies", v.VerifyBuffer<mn::EntityEnter>(nullptr));
        const auto* e = fb::GetRoot<mn::EntityEnter>(buf.data());

        check("3: NPC (char_class 0) still carries appearance", e->char_class() == 0);
        check("3: race round-trips for an appearance NPC", e->race() == 1);
        check("3: sex round-trips for an appearance NPC", e->sex() == 0);
        const auto* a = e->appearance();
        if (check("3: appearance present for an appearance NPC", a != nullptr)) {
            check("3: appearance.version", a->version() == 1);
            check("3: appearance.hair", a->hair() == 1);
            check("3: appearance.face", a->face() == 1);
            check("3: appearance.skin", a->skin() == 1);
        }
        // No worn gear projected this story — the equipment set is empty (a present-
        // but-empty vector when visual is set; worn_items is not projected at M1).
        check("3: no worn items (worn_items not projected at M1)",
              e->equipment() == nullptr || e->equipment()->size() == 0);
    }

    if (g_fail == 0) {
        std::printf("PASS: EntityEnter visual broadcast (player round-trip + NPC absent)\n");
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
