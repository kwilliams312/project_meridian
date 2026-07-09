// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor — server-authoritative buy/sell/buyback (ECO-01; #370).
// See vendor.h for the two-layer (pure plan + DB apply) design + price authority.

#include "vendor.h"

#include <optional>
#include <string>

namespace meridian::vendor {

namespace {

// First empty backpack index, or nullopt when the backpack is full. A purchased /
// repurchased stack always takes a fresh free slot (M1 does not merge into an
// existing partial stack), so this is the placement target for buy + buyback.
std::optional<std::uint16_t> first_free_backpack_slot(const items::Inventory& inv) {
    const auto& slots = inv.backpack();
    for (std::uint16_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].has_value()) return i;
    }
    return std::nullopt;
}

// price * count with an int64 overflow guard (unreachable at M1 prices, but the
// economy must never silently wrap). Throws items::CurrencyOverflow on overflow.
Copper checked_mul(Copper price, std::uint32_t count) {
    const Copper n = static_cast<Copper>(count);
    if (price != 0 && n > items::kMaxCopper / price) {
        throw items::CurrencyOverflow(price, n);
    }
    return price * n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure planners
// ---------------------------------------------------------------------------

BuyPlan plan_buy(const items::Inventory& inv, Copper balance,
                 const VendorCatalog& catalog, const items::TemplateStore& templates,
                 std::uint32_t vendor_id, std::uint32_t item_template_id,
                 std::uint32_t quantity) {
    // The vendor must exist and sell this template.
    if (catalog.listings(vendor_id) == nullptr) throw UnknownVendor(vendor_id);
    const VendorListing* listing = catalog.find_listing(vendor_id, item_template_id);
    if (listing == nullptr) throw ItemNotSold(vendor_id, item_template_id);

    const items::ItemTemplate* tmpl = templates.find(item_template_id);
    if (tmpl == nullptr) throw items::UnknownTemplate(item_template_id);

    // Server-authoritative price (override or template buy_price). No price => the
    // item is stocked but not purchasable — treat as NOT_SOLD.
    std::optional<Copper> unit = VendorCatalog::buy_price(*listing, templates);
    if (!unit.has_value()) throw ItemNotSold(vendor_id, item_template_id);

    // A purchase is a single stack within the template's max_stack.
    if (quantity == 0 || quantity > tmpl->max_stack) {
        throw items::BadStackCount("buy quantity " + std::to_string(quantity) +
                                   " not in [1, " + std::to_string(tmpl->max_stack) + "]");
    }

    const Copper total = checked_mul(*unit, quantity);

    // Funds first (non-mutating comparison), then space — both before any mutation.
    if (balance < total) throw items::InsufficientFunds(balance, total);
    std::optional<std::uint16_t> slot = first_free_backpack_slot(inv);
    if (!slot.has_value()) throw items::InventoryFull();

    BuyPlan p;
    p.item_template_id = item_template_id;
    p.quantity = quantity;
    p.backpack_slot = *slot;
    p.unit_price = *unit;
    p.total_price = total;
    p.balance_after = balance - total;
    return p;
}

SellPlan plan_sell(const items::Inventory& inv, Copper balance,
                   const items::TemplateStore& templates,
                   std::uint16_t backpack_slot, std::uint32_t quantity) {
    const items::ItemInstance* inst = inv.backpack_at(backpack_slot);
    if (inst == nullptr) throw items::SlotEmpty();

    const items::ItemTemplate* tmpl = templates.find(inst->template_id);
    if (tmpl == nullptr) throw items::UnknownTemplate(inst->template_id);
    if (!tmpl->sell_price.has_value()) throw NotSellable(inst->template_id);
    if (quantity == 0) throw items::BadStackCount("sell quantity must be >= 1");

    const std::uint32_t sell_qty = quantity < inst->stack ? quantity : inst->stack;
    const Copper credit = checked_mul(*tmpl->sell_price, sell_qty);
    const Copper after = items::checked_add(balance, credit);  // guards overflow

    SellPlan p;
    p.backpack_slot = backpack_slot;
    p.item_template_id = inst->template_id;
    p.item_guid = inst->item_guid;
    p.quantity = sell_qty;
    p.remove_whole_stack = (sell_qty == inst->stack);
    p.unit_price = *tmpl->sell_price;
    p.total_credit = credit;
    p.balance_after = after;
    return p;
}

BuybackPlan plan_buyback(const items::Inventory& inv, Copper balance,
                         const BuybackQueue& queue, std::uint16_t buyback_slot) {
    const BuybackEntry& e = queue.at(buyback_slot);  // throws BuybackSlotEmpty

    if (balance < e.price) throw items::InsufficientFunds(balance, e.price);
    std::optional<std::uint16_t> slot = first_free_backpack_slot(inv);
    if (!slot.has_value()) throw items::InventoryFull();

    BuybackPlan p;
    p.item_template_id = e.item_template_id;
    p.quantity = e.quantity;
    p.backpack_slot = *slot;
    p.price = e.price;
    p.balance_after = balance - e.price;
    return p;
}

// ---------------------------------------------------------------------------
// Pure apply (mutate the in-memory snapshot)
// ---------------------------------------------------------------------------

namespace {

// Place a freshly-created stack of `template_id` x `stack` at a known-free
// backpack `index`. Uses the trusted load path (inventory.h): the item was just
// minted by a server-validated transaction (space + quantity already checked), so
// this is a legitimate server-decided placement into a verified-empty slot.
void place_new_stack(items::Inventory& inv, std::uint16_t index,
                     std::uint32_t template_id, std::uint32_t stack,
                     std::uint64_t item_guid = 0) {
    items::ItemInstance inst;
    inst.item_guid = item_guid;
    inst.template_id = template_id;
    inst.stack = stack;
    inv.load_backpack(index, inst);
}

}  // namespace

BuyPlan buy(items::Inventory& inv, Copper balance, const VendorCatalog& catalog,
            const items::TemplateStore& templates, std::uint32_t vendor_id,
            std::uint32_t item_template_id, std::uint32_t quantity) {
    BuyPlan p = plan_buy(inv, balance, catalog, templates, vendor_id,
                         item_template_id, quantity);
    place_new_stack(inv, p.backpack_slot, p.item_template_id, p.quantity);
    return p;
}

SellResult sell(items::Inventory& inv, Copper balance, BuybackQueue& queue,
                const items::TemplateStore& templates, std::uint16_t backpack_slot,
                std::uint32_t quantity) {
    SellPlan p = plan_sell(inv, balance, templates, backpack_slot, quantity);
    inv.remove_from_backpack(p.backpack_slot, p.quantity);
    const std::size_t slot =
        queue.push(BuybackEntry{p.item_template_id, p.quantity, p.total_credit});

    SellResult r;
    static_cast<SellPlan&>(r) = p;
    r.buyback_slot = slot;
    return r;
}

BuybackPlan buyback(items::Inventory& inv, Copper balance, BuybackQueue& queue,
                    const items::TemplateStore& templates, std::uint16_t buyback_slot) {
    (void)templates;
    BuybackPlan p = plan_buyback(inv, balance, queue, buyback_slot);
    place_new_stack(inv, p.backpack_slot, p.item_template_id, p.quantity);
    queue.take(buyback_slot);
    return p;
}

// ---------------------------------------------------------------------------
// DB-backed operations (characters DB — currency.h + item_store.h primitives)
// ---------------------------------------------------------------------------

BuyDbResult buy_db(db::Connection& conn, std::uint64_t char_id,
                   const VendorCatalog& catalog, const items::TemplateStore& templates,
                   std::uint32_t vendor_id, std::uint32_t item_template_id,
                   std::uint32_t quantity, std::uint16_t backpack_capacity) {
    items::Inventory inv =
        items::load_inventory(conn, char_id, templates, backpack_capacity);
    const Copper balance = items::get_money(conn, char_id);

    // Validate + price against the durable snapshot (throws on any rejection —
    // nothing has been written yet).
    BuyPlan plan = plan_buy(inv, balance, catalog, templates, vendor_id,
                            item_template_id, quantity);

    // Debit first: subtract_money is one row-locked transaction that re-checks
    // affordability atomically (a lost race throws InsufficientFunds, money
    // unchanged). Then mint + place; on a mint/place fault refund the debit so the
    // balance is never left short of an item.
    items::subtract_money(conn, char_id, plan.total_price);
    items::ItemInstance minted;
    try {
        minted = items::mint_instance(conn, plan.item_template_id, plan.quantity);
        items::place_item(conn, char_id, /*bag=*/0,
                          items::backpack_placement_slot(plan.backpack_slot),
                          minted.item_guid);
    } catch (...) {
        items::add_money(conn, char_id, plan.total_price);  // refund
        throw;
    }

    BuyDbResult r;
    static_cast<BuyPlan&>(r) = plan;
    r.item_guid = minted.item_guid;
    return r;
}

SellDbResult sell_db(db::Connection& conn, std::uint64_t char_id, BuybackQueue& queue,
                     const items::TemplateStore& templates, std::uint16_t backpack_slot,
                     std::uint32_t quantity, std::uint16_t backpack_capacity) {
    items::Inventory inv =
        items::load_inventory(conn, char_id, templates, backpack_capacity);
    const Copper balance = items::get_money(conn, char_id);

    SellPlan plan = plan_sell(inv, balance, templates, backpack_slot, quantity);

    // Credit first (add_money is a checked, row-locked transaction — the only
    // reversible-if-needed step), then remove the item durably; on a remove fault
    // reverse the credit so money and items stay consistent.
    items::add_money(conn, char_id, plan.total_credit);
    try {
        if (plan.remove_whole_stack) {
            items::clear_placement(conn, char_id, plan.item_guid);
            items::destroy_instance(conn, plan.item_guid);
        } else {
            const items::ItemInstance* inst = inv.backpack_at(backpack_slot);
            items::set_instance_stack(conn, plan.item_guid, inst->stack - plan.quantity);
        }
    } catch (...) {
        items::subtract_money(conn, char_id, plan.total_credit);  // reverse credit
        throw;
    }

    const std::size_t slot =
        queue.push(BuybackEntry{plan.item_template_id, plan.quantity, plan.total_credit});

    SellDbResult r;
    static_cast<SellPlan&>(r) = plan;
    r.buyback_slot = slot;
    return r;
}

BuybackDbResult buyback_db(db::Connection& conn, std::uint64_t char_id,
                           BuybackQueue& queue, const items::TemplateStore& templates,
                           std::uint16_t buyback_slot, std::uint16_t backpack_capacity) {
    items::Inventory inv =
        items::load_inventory(conn, char_id, templates, backpack_capacity);
    const Copper balance = items::get_money(conn, char_id);

    BuybackPlan plan = plan_buyback(inv, balance, queue, buyback_slot);

    items::subtract_money(conn, char_id, plan.price);
    items::ItemInstance minted;
    try {
        minted = items::mint_instance(conn, plan.item_template_id, plan.quantity);
        items::place_item(conn, char_id, /*bag=*/0,
                          items::backpack_placement_slot(plan.backpack_slot),
                          minted.item_guid);
    } catch (...) {
        items::add_money(conn, char_id, plan.price);  // refund
        throw;
    }

    queue.take(buyback_slot);  // consume only after the durable restore succeeded

    BuybackDbResult r;
    static_cast<BuybackPlan&>(r) = plan;
    r.item_guid = minted.item_guid;
    return r;
}

}  // namespace meridian::vendor
