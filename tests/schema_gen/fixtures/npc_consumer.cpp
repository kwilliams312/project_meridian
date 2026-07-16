// Consumption proof for the generated C++ typed content model.
//
// Populates an mcc::content::Npc from the real content entity
// content/core/npcs/quartermaster_bren.npc.yaml BY HAND (no YAML decoder yet —
// that is a later mcc task), asserting the generated struct has exactly the
// fields/types needed to represent a real NPC. If the generated header did not
// compile under C++20, or a field were missing/mistyped, this file would not
// build. Exit 0 = the typed model faithfully represents real content.

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>

#include "content_types.gen.hpp"

using namespace mcc::content;

int main() {
    Npc bren;
    bren.id = ContentId{"core:npc.quartermaster_bren"};
    bren.name = "Quartermaster Bren";
    bren.subtitle = "Emberfall Supplies";
    bren.level = IntRange{10, 10};
    bren.creature_type = NpcCreatureType::Humanoid;
    bren.faction = NpcFaction::Friendly;

    NpcStats stats;
    stats.health = 800;
    stats.armor = 200;               // optional<int64_t>
    stats.damage = IntRange{15, 22};
    stats.attack_speed_ms = 2000;
    bren.stats = stats;

    NpcInteraction interaction;
    interaction.gossip_text = "Mind the mine roads, traveler ...";
    interaction.vendor = VendorRef{"vendor.bren_general_goods"};
    bren.interaction = interaction;

    NpcVisual visual;
    visual.model = ArtRef{"core:art.char.human.male.quartermaster"};
    visual.sound_set = SfxRef{"core:sfx.char.human_male"};
    bren.visual = visual;

    // The values round-trip back out with the expected types.
    assert(bren.id.id == "core:npc.quartermaster_bren");
    assert(bren.level.min == 10 && bren.level.max == 10);
    assert(bren.creature_type == NpcCreatureType::Humanoid);
    assert(bren.stats.health == 800);
    assert(bren.stats.armor.has_value() && *bren.stats.armor == 200);
    assert(bren.stats.damage.max == 22);
    assert(bren.interaction.has_value());
    assert(bren.interaction->vendor.has_value());
    // npc@2: visual is model-XOR-appearance, so `model` is now optional (branch A).
    assert(bren.visual->model.has_value() &&
           bren.visual->model->id == "core:art.char.human.male.quartermaster");
    assert(!bren.visual->appearance.has_value());  // branch B unused for this NPC

    // Branch B (assemble-like-a-player) round-trips its own optional fields.
    NpcVisual appearance_visual;
    appearance_visual.appearance = AppearanceRef{"chibi:appearance.red.male"};
    assert(appearance_visual.appearance.has_value() &&
           appearance_visual.appearance->id == "chibi:appearance.red.male");
    assert(!appearance_visual.model.has_value());

    // Optional-when-absent fields are correctly empty.
    assert(!bren.rank.has_value());
    assert(!bren.ai.has_value());
    assert(!bren.loot.has_value());
    assert(!bren.stats.mana.has_value());

    return 0;
}
