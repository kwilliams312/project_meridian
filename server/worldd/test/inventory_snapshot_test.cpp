// SPDX-License-Identifier: Apache-2.0
//
// worldd — INVENTORY_SNAPSHOT own-equipped-set UNIT TEST (#867, epic #866).
// Proves the additive `equipped` field encode_inventory_snapshot projects — the
// client's ONLY source for its OWN gear (EquippedVisual on EntityEnter describes
// OTHER entities, and INVENTORY_SNAPSHOT carried backpack + money only until now):
//
//   * a character with a populated paperdoll reports EVERY occupied EquipSlot,
//     keyed by its EquipSlot id, with the right item_template;
//   * JEWELLERY (kFinger) rides here even though EntityEnter's EquippedVisual
//     deliberately omits it — this snapshot is pushed only to the OWNING client,
//     so there is no observer to leak equipment state to (the whole reason the
//     is_visual_equip_slot filter exists). This is the assertion that would catch
//     someone "reusing" visible_equipment_visuals for this field;
//   * EMPTY paperdoll positions are OMITTED entirely (never a 0 item_template),
//     so `equipped` is sparse and a bare character yields an EMPTY vector;
//   * the pre-existing money / backpack / backpack_slots projection is unchanged
//     by the addition (no behaviour change beyond the added data).
//
// CLEAN-ROOM: written from world.fbs' additive-field contract + the items lib's
// public API (inventory.h); no GPL source consulted (CONTRIBUTING.md).
//
// DB-free: drives encode_inventory_snapshot() directly against a hand-seeded
// items::Inventory (the trusted load_* path the DB loader uses) — no TLS session,
// no MariaDB. Runs in the plain ctest.

#include "inventory.h"       // items::Inventory / EquipSlot / ItemInstance
#include "item_template.h"   // items::PlaceholderTemplateStore
#include "world_dispatch.h"  // encode_inventory_snapshot
#include "world_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace meridian::worldd;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace itm = meridian::items;

namespace {

int g_fail = 0;
bool check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
    return ok;
}

// Placeholder templates (item_template.cpp): 900004 Rugged Leather Vest (kChest),
// 900005 Simple Cloth Cap (kHead), 900006 Traveler's Ring (kFinger — JEWELLERY),
// 900001 Worn Shortsword (kMainHand), 900007 Minor Health Potion (stackable).
constexpr std::uint32_t kVest = 900004;
constexpr std::uint32_t kCap = 900005;
constexpr std::uint32_t kRing = 900006;
constexpr std::uint32_t kSword = 900001;
constexpr std::uint32_t kPotion = 900007;

itm::ItemInstance inst(std::uint64_t guid, std::uint32_t tmpl, std::uint32_t stack = 1) {
    itm::ItemInstance i;
    i.item_guid = guid;
    i.template_id = tmpl;
    i.stack = stack;
    return i;
}

// Decode a snapshot payload back to the verified table (the client's view). The
// returned pointer is a VIEW into `buf`, so callers must keep `buf` alive for as
// long as they read the table (never `decode(encode_...())` — that dangles).
const mn::InventorySnapshot* decode(const std::vector<std::uint8_t>& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<mn::InventorySnapshot>(nullptr)) return nullptr;
    return fb::GetRoot<mn::InventorySnapshot>(buf.data());
}

// The item_template at EquipSlot `slot` in a decoded snapshot, or 0 when absent.
std::uint32_t equipped_template(const mn::InventorySnapshot* m, itm::EquipSlot slot) {
    if (m == nullptr || m->equipped() == nullptr) return 0;
    for (const auto* e : *m->equipped()) {
        if (e != nullptr && e->slot() == static_cast<std::uint8_t>(slot)) return e->item_template();
    }
    return 0;
}

// --- a populated paperdoll reports every occupied slot ------------------------

void test_equipped_set_projected() {
    std::puts("[snapshot] a character's OWN equipped set rides on INVENTORY_SNAPSHOT");

    const itm::PlaceholderTemplateStore templates;
    itm::Inventory inv(templates, /*backpack_capacity=*/16);
    inv.load_equipment(itm::EquipSlot::kHead, inst(11, kCap));
    inv.load_equipment(itm::EquipSlot::kChest, inst(12, kVest));
    inv.load_equipment(itm::EquipSlot::kFinger, inst(13, kRing));   // JEWELLERY
    inv.load_equipment(itm::EquipSlot::kMainHand, inst(14, kSword));

    const auto buf = encode_inventory_snapshot(/*money=*/4200, inv);
    const auto* m = decode(buf);
    if (!check("snapshot verifies", m != nullptr)) return;

    check("equipped carries exactly the 4 occupied positions",
          m->equipped() != nullptr && m->equipped()->size() == 4);
    check("kHead -> Simple Cloth Cap", equipped_template(m, itm::EquipSlot::kHead) == kCap);
    check("kChest -> Rugged Leather Vest", equipped_template(m, itm::EquipSlot::kChest) == kVest);
    check("kMainHand -> Worn Shortsword",
          equipped_template(m, itm::EquipSlot::kMainHand) == kSword);
    // The load-bearing one: EntityEnter's EquippedVisual OMITS jewellery (it is an AoI
    // broadcast); this owner-only snapshot MUST carry it or the sheet cannot show rings.
    check("kFinger (JEWELLERY) rides on the owner-only snapshot",
          equipped_template(m, itm::EquipSlot::kFinger) == kRing);

    // dyes are empty at M1 (no dye-application path writes item_instance.dyes yet).
    bool dyes_empty = true;
    for (const auto* e : *m->equipped())
        if (e->dyes() != nullptr && e->dyes()->size() != 0) dyes_empty = false;
    check("dyes empty at M1 (authored colours)", dyes_empty);
}

// --- empty paperdoll positions are omitted ------------------------------------

void test_empty_slots_omitted() {
    std::puts("[snapshot] EMPTY paperdoll positions are omitted (never a 0 template)");

    const itm::PlaceholderTemplateStore templates;
    itm::Inventory inv(templates, /*backpack_capacity=*/16);
    inv.load_equipment(itm::EquipSlot::kChest, inst(21, kVest));  // one slot only

    const auto buf = encode_inventory_snapshot(/*money=*/0, inv);
    const auto* m = decode(buf);
    if (!check("snapshot verifies", m != nullptr)) return;

    check("only the ONE occupied position is emitted",
          m->equipped() != nullptr && m->equipped()->size() == 1);
    check("the emitted row is kChest", equipped_template(m, itm::EquipSlot::kChest) == kVest);
    // Sparse, not zero-filled: an unoccupied slot has no row at all.
    check("kHead absent (not a 0-template row)",
          equipped_template(m, itm::EquipSlot::kHead) == 0);
    bool no_zero_templates = true;
    for (const auto* e : *m->equipped())
        if (e->item_template() == 0) no_zero_templates = false;
    check("no row carries item_template 0", no_zero_templates);
}

void test_bare_character() {
    std::puts("[snapshot] a BARE character yields an empty equipped vector, not a failure");

    const itm::PlaceholderTemplateStore templates;
    const itm::Inventory inv(templates, /*backpack_capacity=*/16);

    const auto buf = encode_inventory_snapshot(/*money=*/0, inv);
    const auto* m = decode(buf);
    if (!check("snapshot verifies", m != nullptr)) return;
    check("equipped is empty", m->equipped() != nullptr && m->equipped()->size() == 0);
}

// --- the pre-existing projection is unchanged --------------------------------

void test_backpack_projection_unchanged() {
    std::puts("[snapshot] money + backpack projection unchanged by the addition (#867)");

    const itm::PlaceholderTemplateStore templates;
    itm::Inventory inv(templates, /*backpack_capacity=*/16);
    inv.load_backpack(/*index=*/0, inst(31, kPotion, /*stack=*/5));
    inv.load_backpack(/*index=*/3, inst(32, kSword));
    inv.load_equipment(itm::EquipSlot::kChest, inst(33, kVest));

    const auto buf = encode_inventory_snapshot(/*money=*/4200, inv);
    const auto* m = decode(buf);
    if (!check("snapshot verifies", m != nullptr)) return;

    check("money round-trips", m->money() == 4200);
    check("backpack_slots round-trips", m->backpack_slots() == 16);
    if (!check("only the 2 OCCUPIED backpack slots are emitted",
               m->items() != nullptr && m->items()->size() == 2))
        return;
    check("items[0] = the potion stack at grid 0",
          m->items()->Get(0)->slot() == 0 && m->items()->Get(0)->item_template_id() == kPotion &&
              m->items()->Get(0)->count() == 5);
    check("items[1] = the sword at grid 3",
          m->items()->Get(1)->slot() == 3 && m->items()->Get(1)->item_template_id() == kSword);
    // Equipment and backpack are DISTINCT projections — an equipped item must not
    // leak into `items` (they share bag 0 in the DB but not on the wire).
    check("the equipped vest does NOT appear in the backpack items",
          m->items()->Get(0)->item_template_id() != kVest &&
              m->items()->Get(1)->item_template_id() != kVest);
    check("the equipped vest DOES appear in equipped",
          equipped_template(m, itm::EquipSlot::kChest) == kVest);
}

}  // namespace

int main() {
    std::puts("worldd INVENTORY_SNAPSHOT own-equipped-set test (#867)");
    test_equipped_set_projected();
    test_empty_slots_omitted();
    test_bare_character();
    test_backpack_projection_unchanged();
    if (g_fail != 0) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}
