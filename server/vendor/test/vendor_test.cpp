// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor DB-FREE unit test (ECO-01; issue #370).
//
// The deterministic proof of the server-authoritative economy rules, with no DB:
//   * BuybackQueue      — bounded FIFO, oldest-out eviction, slot take.
//   * VendorCatalog     — listing lookup + price resolution (override + template
//                         buy_price, and the "not purchasable" case).
//   * buy               — debits copper + grants the item; rejects insufficient
//                         funds, insufficient space, an unsold item, an unknown
//                         vendor, and a bad quantity — each leaving state unchanged.
//   * sell              — credits copper + removes the item (whole + partial stack);
//                         pushes the sold stack onto the buyback queue; rejects an
//                         unsellable item and an empty slot.
//   * buyback           — restores a sold item at its sale price + re-debits; rejects
//                         insufficient funds and an empty buyback slot.
//
// Clean-room, original code (CONTRIBUTING.md).

#include <cstdio>

#include "buyback.h"
#include "inventory.h"
#include "item_template.h"
#include "vendor.h"
#include "vendor_catalog.h"

using namespace meridian;
using namespace meridian::vendor;
using items::Copper;

namespace {

int g_fail = 0;

void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

template <typename E, typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

constexpr std::uint32_t B = items::kPlaceholderIdBase;
constexpr std::uint32_t kSword = B + 1;    // buy 100, sell 25, max_stack 1
constexpr std::uint32_t kStaff = B + 3;    // NOT sold by the placeholder vendor
constexpr std::uint32_t kPotion = B + 7;   // buy 5,  sell 1,  max_stack 20
constexpr std::uint32_t kOre = B + 8;      // no template buy_price; catalog override 6; sell 2
constexpr std::uint32_t kQuest = B + 9;    // unsellable (no sell_price)
constexpr std::uint32_t kVendor = kPlaceholderGeneralVendor;

items::ItemInstance make(std::uint32_t template_id, std::uint32_t stack,
                         std::uint64_t guid) {
    items::ItemInstance i;
    i.item_guid = guid;
    i.template_id = template_id;
    i.stack = stack;
    return i;
}

// ---------------------------------------------------------------------------
void test_buyback_queue() {
    std::printf("[buyback queue]\n");
    BuybackQueue q(2);
    check("empty queue size 0", q.size() == 0 && q.empty());
    check("push returns slot 0", q.push(BuybackEntry{kSword, 1, 25}) == 0);
    check("push returns slot 1", q.push(BuybackEntry{kPotion, 5, 5}) == 1);
    check("size at capacity", q.size() == 2);
    // Third push evicts the oldest (slot 0 == sword); potion shifts to slot 0.
    check("push past capacity returns slot 1", q.push(BuybackEntry{kOre, 3, 6}) == 1);
    check("oldest evicted (slot 0 now potion)", q.at(0).item_template_id == kPotion);
    check("newest at slot 1 (ore)", q.at(1).item_template_id == kOre);
    check("out-of-range slot throws", throws<BuybackSlotEmpty>([&] { q.at(2); }));
    BuybackEntry taken = q.take(0);
    check("take returns the entry", taken.item_template_id == kPotion);
    check("take shrinks + shifts (ore now slot 0)",
          q.size() == 1 && q.at(0).item_template_id == kOre);
}

// ---------------------------------------------------------------------------
void test_catalog() {
    std::printf("[catalog]\n");
    items::PlaceholderTemplateStore tmpl;
    PlaceholderVendorCatalog cat;

    check("known vendor has listings", cat.listings(kVendor) != nullptr);
    check("unknown vendor -> nullptr", cat.listings(4242u) == nullptr);
    check("vendor sells the sword", cat.find_listing(kVendor, kSword) != nullptr);
    check("vendor does not sell the staff", cat.find_listing(kVendor, kStaff) == nullptr);

    const VendorListing* sword = cat.find_listing(kVendor, kSword);
    check("sword price = template buy_price (100)",
          VendorCatalog::buy_price(*sword, tmpl) == Copper{100});
    const VendorListing* ore = cat.find_listing(kVendor, kOre);
    check("ore price = catalog override (6, template has none)",
          VendorCatalog::buy_price(*ore, tmpl) == Copper{6});
}

// ---------------------------------------------------------------------------
void test_buy() {
    std::printf("[buy]\n");
    items::PlaceholderTemplateStore tmpl;
    PlaceholderVendorCatalog cat;

    // Debits copper + grants the item.
    {
        items::Inventory inv(tmpl);
        BuyPlan p = buy(inv, /*balance=*/1000, cat, tmpl, kVendor, kSword, 1);
        check("buy debits the sword price", p.total_price == Copper{100});
        check("buy returns the debited balance", p.balance_after == Copper{900});
        check("bought item lands in a backpack slot",
              inv.backpack_at(p.backpack_slot) != nullptr &&
                  inv.backpack_at(p.backpack_slot)->template_id == kSword);
        check("bought stack quantity is 1",
              inv.backpack_at(p.backpack_slot)->stack == 1);
        check("backpack now holds one item", inv.backpack_used() == 1);
    }
    // A stack purchase (5 potions at 5 each = 25).
    {
        items::Inventory inv(tmpl);
        BuyPlan p = buy(inv, 1000, cat, tmpl, kVendor, kPotion, 5);
        check("buy 5 potions costs 25", p.total_price == Copper{25});
        check("potion stack of 5 granted",
              inv.backpack_at(p.backpack_slot) &&
                  inv.backpack_at(p.backpack_slot)->stack == 5);
    }
    // Insufficient funds — rejected, nothing granted.
    {
        items::Inventory inv(tmpl);
        check("buy with too little copper throws InsufficientFunds",
              throws<items::InsufficientFunds>(
                  [&] { buy(inv, 50, cat, tmpl, kVendor, kSword, 1); }));
        check("no item granted on failed buy", inv.backpack_used() == 0);
    }
    // Insufficient space — a full backpack rejects the buy.
    {
        items::Inventory inv(tmpl, /*backpack_capacity=*/1);
        inv.add(make(kSword, 1, 111));  // fill the only slot
        check("buy into a full backpack throws InventoryFull",
              throws<items::InventoryFull>(
                  [&] { buy(inv, 1000, cat, tmpl, kVendor, kPotion, 1); }));
    }
    // Item the vendor does not sell / unknown vendor / bad quantity.
    {
        items::Inventory inv(tmpl);
        check("buying an unsold item throws ItemNotSold",
              throws<ItemNotSold>([&] { buy(inv, 1000, cat, tmpl, kVendor, kStaff, 1); }));
        check("unknown vendor throws UnknownVendor",
              throws<UnknownVendor>([&] { buy(inv, 1000, cat, tmpl, 4242u, kSword, 1); }));
        check("quantity 0 throws BadStackCount",
              throws<items::BadStackCount>(
                  [&] { buy(inv, 1000, cat, tmpl, kVendor, kPotion, 0); }));
        check("quantity above max_stack throws BadStackCount",
              throws<items::BadStackCount>(
                  [&] { buy(inv, 1000, cat, tmpl, kVendor, kPotion, 999); }));
    }
}

// ---------------------------------------------------------------------------
void test_sell() {
    std::printf("[sell]\n");
    items::PlaceholderTemplateStore tmpl;

    // Credits copper + removes the whole stack; pushes onto buyback.
    {
        items::Inventory inv(tmpl);
        inv.add(make(kSword, 1, 501));
        BuybackQueue q;
        SellResult r = sell(inv, /*balance=*/0, q, tmpl, /*slot=*/0, /*qty=*/1);
        check("sell credits the sell price (25)", r.total_credit == Copper{25});
        check("sell returns the credited balance", r.balance_after == Copper{25});
        check("sold slot is now empty", inv.backpack_at(0) == nullptr);
        check("sold stack pushed onto buyback", q.size() == 1);
        check("buyback entry matches the sold item",
              q.at(r.buyback_slot).item_template_id == kSword &&
                  q.at(r.buyback_slot).price == Copper{25});
    }
    // Partial-stack sell decrements the stack (whole-stack flag false).
    {
        items::Inventory inv(tmpl);
        inv.add(make(kPotion, 5, 502));
        BuybackQueue q;
        SellResult r = sell(inv, 0, q, tmpl, 0, 2);
        check("partial sell credits qty * price (2)", r.total_credit == Copper{2});
        check("partial sell is not a whole-stack removal", !r.remove_whole_stack);
        check("remaining stack decremented to 3",
              inv.backpack_at(0) && inv.backpack_at(0)->stack == 3);
    }
    // Unsellable item + empty slot.
    {
        items::Inventory inv(tmpl);
        inv.add(make(kQuest, 1, 503));
        BuybackQueue q;
        check("selling an unsellable item throws NotSellable",
              throws<NotSellable>([&] { sell(inv, 0, q, tmpl, 0, 1); }));
        check("selling an empty slot throws SlotEmpty",
              throws<items::SlotEmpty>([&] { sell(inv, 0, q, tmpl, 5, 1); }));
    }
}

// ---------------------------------------------------------------------------
void test_buyback() {
    std::printf("[buyback]\n");
    items::PlaceholderTemplateStore tmpl;

    // Sell then buy back: the item returns and the sale price is re-debited.
    {
        items::Inventory inv(tmpl);
        inv.add(make(kSword, 1, 601));
        BuybackQueue q;
        SellResult sold = sell(inv, 0, q, tmpl, 0, 1);  // balance -> 25, buyback[0] = sword@25
        check("precondition: sword sold, backpack empty", inv.backpack_used() == 0);

        BuybackPlan p = buyback(inv, sold.balance_after, q, tmpl,
                                static_cast<std::uint16_t>(sold.buyback_slot));
        check("buyback re-debits the sale price", p.price == Copper{25});
        check("buyback returns the re-debited balance (0)", p.balance_after == Copper{0});
        check("bought-back item is restored to the backpack",
              inv.backpack_at(p.backpack_slot) &&
                  inv.backpack_at(p.backpack_slot)->template_id == kSword);
        check("buyback entry consumed from the queue", q.empty());
    }
    // Cannot afford the buyback.
    {
        items::Inventory inv(tmpl);
        BuybackQueue q;
        q.push(BuybackEntry{kSword, 1, 25});
        check("buyback with too little copper throws InsufficientFunds",
              throws<items::InsufficientFunds>([&] { buyback(inv, 0, q, tmpl, 0); }));
        check("nothing restored on a failed buyback", inv.backpack_used() == 0);
    }
    // Empty buyback slot.
    {
        items::Inventory inv(tmpl);
        BuybackQueue q;
        check("buyback of an empty slot throws BuybackSlotEmpty",
              throws<BuybackSlotEmpty>([&] { buyback(inv, 1000, q, tmpl, 5); }));
    }
}

}  // namespace

int main() {
    test_buyback_queue();
    test_catalog();
    test_buy();
    test_sell();
    test_buyback();

    std::printf(g_fail == 0 ? "\nALL VENDOR UNIT TESTS PASSED\n"
                            : "\n%d VENDOR UNIT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
