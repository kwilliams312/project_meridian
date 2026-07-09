// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor — server-authoritative vendor transactions: buy / sell /
// buyback (ECO-01; server PRD §4-M1 "vendors: buy/sell/buyback ... server-
// authoritative, int64 copper, no client-trusted prices"; issue #370).
//
// Two layers, mirroring meridian-items (currency.h / item_store.h):
//
//   1. PURE domain logic (DB-free, deterministic). `plan_*` resolve a transaction
//      against an in-memory snapshot — the vendor catalog, the item templates, the
//      character's inventory + copper balance, and (for buyback) the buyback queue
//      — validating EVERYTHING the server owns (does the vendor sell it / does the
//      player own it / can they afford it / is there inventory space) and computing
//      the server price. They MUTATE NOTHING and throw a typed error on any
//      rejection, so a refused transaction is all-or-nothing. `buy`/`sell`/
//      `buyback` are the in-memory apply: plan, then mutate the Inventory + copper
//      + buyback queue. These are unit-tested with no DB — the deterministic proof
//      of the economy rules.
//
//   2. DB-backed operations (`buy_db` / `sell_db` / `buyback_db`) compose the pure
//      plan with meridian-items' durable primitives (currency.h add/subtract_money
//      — single-transaction, row-locked; item_store.h mint/place/destroy) against
//      the characters DB. They are the real server path the worldd handler calls;
//      exercised by the DB integration test.
//
// PRICE AUTHORITY: a request never carries a price. Buy price = the catalog
// listing's override, else the item template's buy_price (vendor_catalog.h). Sell/
// buyback price = the item template's sell_price. All arithmetic is int64 copper
// through meridian-items' checked add/subtract (never a FLOAT, never negative).
//
// PLACEMENT: a bought / repurchased stack is minted as ONE new stack into the
// first free backpack slot (M1 does not merge a purchase into an existing partial
// stack — a purchase is a whole stack, quantity <= the template's max_stack). The
// pure and DB paths place identically so their results agree.
//
// Clean-room, original code (CONTRIBUTING.md — designed from OUR PRD/SAD/DDL only,
// no GPL/AGPL/CMaNGOS/TrinityCore/leaked vendor logic consulted).

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>

#include "buyback.h"
#include "currency.h"
#include "inventory.h"
#include "item_store.h"
#include "item_template.h"
#include "meridian/db/connection.h"
#include "vendor_catalog.h"

namespace meridian::vendor {

using items::Copper;

// --- Errors ------------------------------------------------------------------
// Vendor-specific rejections. Funds / space / quantity / slot rejections reuse
// the meridian-items typed errors (items::InsufficientFunds, items::InventoryFull,
// items::BadStackCount, items::SlotEmpty, items::UnknownTemplate,
// items::CurrencyOverflow) so a caller catches ONE taxonomy across the item +
// vendor layers. Buyback slot errors are vendor::BuybackSlotEmpty (buyback.h).

class VendorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// No such vendor id in the catalog.
class UnknownVendor : public VendorError {
public:
    explicit UnknownVendor(std::uint32_t vendor_id)
        : VendorError("unknown vendor: " + std::to_string(vendor_id)) {}
};

// The vendor does not sell that template (no listing, or the listing/template has
// no purchase price).
class ItemNotSold : public VendorError {
public:
    ItemNotSold(std::uint32_t vendor_id, std::uint32_t template_id)
        : VendorError("vendor " + std::to_string(vendor_id) +
                      " does not sell template " + std::to_string(template_id)) {}
};

// The item has no sell price (e.g. a quest item) — a vendor will not buy it.
class NotSellable : public VendorError {
public:
    explicit NotSellable(std::uint32_t template_id)
        : VendorError("template " + std::to_string(template_id) +
                      " has no sell price (unsellable)") {}
};

// --- Transaction plans (pure, non-mutating results of validation) ------------

struct BuyPlan {
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    std::uint16_t backpack_slot = 0;  // the free backpack index the stack will occupy
    Copper unit_price = 0;
    Copper total_price = 0;           // unit_price * quantity
    Copper balance_after = 0;         // balance - total_price
};

struct SellPlan {
    std::uint16_t backpack_slot = 0;
    std::uint32_t item_template_id = 0;
    std::uint64_t item_guid = 0;      // the instance being sold (from the loaded inventory)
    std::uint32_t quantity = 0;       // units sold (clamped to the stack)
    bool remove_whole_stack = false;  // true if quantity == the full stack (slot clears)
    Copper unit_price = 0;
    Copper total_credit = 0;          // unit_price * quantity
    Copper balance_after = 0;         // balance + total_credit
};

struct BuybackPlan {
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    std::uint16_t backpack_slot = 0;  // free backpack index the restored stack occupies
    Copper price = 0;
    Copper balance_after = 0;         // balance - price
};

// --- Pure planners (no mutation; throw a typed error on rejection) ------------

// Validate + price a BUY of `quantity` of `item_template_id` from `vendor_id`
// against `inv` (space) and `balance` (funds). Throws UnknownVendor / ItemNotSold
// / items::UnknownTemplate / items::BadStackCount (quantity 0 or > max_stack) /
// items::CurrencyOverflow / items::InsufficientFunds / items::InventoryFull.
BuyPlan plan_buy(const items::Inventory& inv, Copper balance,
                 const VendorCatalog& catalog, const items::TemplateStore& templates,
                 std::uint32_t vendor_id, std::uint32_t item_template_id,
                 std::uint32_t quantity);

// Validate + price a SELL of `quantity` units from backpack `backpack_slot`.
// Throws items::SlotEmpty / items::UnknownTemplate / NotSellable /
// items::BadStackCount (quantity 0) / items::CurrencyOverflow.
SellPlan plan_sell(const items::Inventory& inv, Copper balance,
                   const items::TemplateStore& templates,
                   std::uint16_t backpack_slot, std::uint32_t quantity);

// Validate a BUYBACK of the queue entry at `buyback_slot` against `inv` (space)
// and `balance` (funds). Throws BuybackSlotEmpty / items::InsufficientFunds /
// items::InventoryFull.
BuybackPlan plan_buyback(const items::Inventory& inv, Copper balance,
                         const BuybackQueue& queue, std::uint16_t buyback_slot);

// --- Pure apply (mutates the in-memory snapshot; deterministic, DB-free) ------

// Apply a buy: mint the purchased stack into the planned free backpack slot and
// return the plan (with balance_after debited). The caller owns `balance` — read
// plan.balance_after for the new value.
BuyPlan buy(items::Inventory& inv, Copper balance, const VendorCatalog& catalog,
            const items::TemplateStore& templates, std::uint32_t vendor_id,
            std::uint32_t item_template_id, std::uint32_t quantity);

// Apply a sell: remove the units from the backpack, credit `balance` (read
// plan.balance_after), and push the sold stack onto `queue`. Returns the plan
// with `buyback_slot` set (below) to where it landed in the queue.
struct SellResult : SellPlan {
    std::size_t buyback_slot = 0;  // index of the pushed entry in the buyback queue
};
SellResult sell(items::Inventory& inv, Copper balance, BuybackQueue& queue,
                const items::TemplateStore& templates, std::uint16_t backpack_slot,
                std::uint32_t quantity);

// Apply a buyback: mint the restored stack into the planned free backpack slot,
// debit `balance` (read plan.balance_after), and remove the entry from `queue`.
BuybackPlan buyback(items::Inventory& inv, Copper balance, BuybackQueue& queue,
                    const items::TemplateStore& templates, std::uint16_t buyback_slot);

// --- DB-backed operations (the real server path; characters DB) --------------
// Each loads the character's inventory + money from `conn`, plans the transaction,
// and applies it durably (currency.h single-transaction money ops + item_store.h
// mint/place/destroy). `char_id` is the owning character (server-authoritative —
// the handler supplies the session's own character). Throw the same typed errors
// as the planners, plus items::CharacterNotFound / db::DbError on a DB fault. A
// throw leaves durable state consistent (money never partially applied).

struct BuyDbResult : BuyPlan {
    std::uint64_t item_guid = 0;  // the minted instance's server guid
};
BuyDbResult buy_db(db::Connection& conn, std::uint64_t char_id,
                   const VendorCatalog& catalog, const items::TemplateStore& templates,
                   std::uint32_t vendor_id, std::uint32_t item_template_id,
                   std::uint32_t quantity,
                   std::uint16_t backpack_capacity = items::kDefaultBackpackSlots);

struct SellDbResult : SellPlan {
    std::size_t buyback_slot = 0;
};
SellDbResult sell_db(db::Connection& conn, std::uint64_t char_id, BuybackQueue& queue,
                     const items::TemplateStore& templates, std::uint16_t backpack_slot,
                     std::uint32_t quantity,
                     std::uint16_t backpack_capacity = items::kDefaultBackpackSlots);

struct BuybackDbResult : BuybackPlan {
    std::uint64_t item_guid = 0;
};
BuybackDbResult buyback_db(db::Connection& conn, std::uint64_t char_id,
                           BuybackQueue& queue, const items::TemplateStore& templates,
                           std::uint16_t buyback_slot,
                           std::uint16_t backpack_capacity = items::kDefaultBackpackSlots);

}  // namespace meridian::vendor
