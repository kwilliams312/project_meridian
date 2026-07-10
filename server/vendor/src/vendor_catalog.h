// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor — the vendor CATALOG seam: what a vendor sells + at what price
// (ECO-01; server PRD §4-M1 "vendors: buy/sell/buyback"; issue #370).
//
// A vendor's for-sale list is READ-ONLY content. It mirrors OUR content schema
// (schema/content/vendor.schema.yaml `meridian/vendor@1`) and its compiled world-
// DB shape (schema/sql/world/60_vendor.sql `vendor_inventory` /
// `vendor_inventory_item`): a vendor is an id, and each item it sells is a row of
// { item template, optional price override, optional limited stock }. The sell
// price for a listing is the override when present, else the item template's
// buy_price (item_template.h); a template with neither is NOT sold (schema lint
// L062).
//
// DATASTORE SEAM (mcc #28): like the item TemplateStore, the catalog is an
// abstract interface so the transaction logic depends only on "what does vendor V
// sell", never on where that comes from. M1 wires a PlaceholderVendorCatalog (a
// small original dev set over the placeholder item templates, vendor_catalog.cpp);
// when mcc #28 compiles vendor.schema.yaml into the world DB, a WorldDbVendorCatalog
// implements the SAME seam and the placeholder set is dropped — no buy/sell/buyback
// code changes.
//
// M1 SCOPE: `limited` stock (count + restock timer) is carried for schema fidelity
// but NOT enforced — placeholder vendors have effectively unlimited stock. Real
// limited-stock depletion needs durable per-vendor state + a restock clock and is
// deferred with the real catalog (mcc #28). The buy path validates only that the
// vendor SELLS the template (a listing exists), which is the M1 requirement.
//
// Clean-room, original code (CONTRIBUTING.md — no GPL/leaked vendor logic).

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "item_template.h"  // meridian::items::Copper, TemplateStore

namespace meridian::vendor {

using items::Copper;

// Optional limited-stock descriptor (vendor.schema.yaml `limited`). Carried for
// schema fidelity; NOT enforced at M1 (see the file header). count>=1,
// restock_minutes>=1 when present.
struct LimitedStock {
    std::uint32_t count = 0;             // units in stock per restock cycle
    std::uint32_t restock_minutes = 0;   // minutes between restocks
};

// One for-sale listing in a vendor's inventory (a vendor_inventory_item row).
struct VendorListing {
    std::uint32_t item_template_id = 0;         // -> world DB item_template (IF-9)
    std::optional<Copper> price_override;        // overrides the template buy_price when set
    std::optional<LimitedStock> limited;         // limited stock (unenforced at M1)
};

// --- The catalog seam --------------------------------------------------------
// Read-only source of vendor inventories. buy/sell/buyback depend ONLY on this
// interface. mcc #28 later adds a world-DB implementation of the same interface.
class VendorCatalog {
public:
    virtual ~VendorCatalog() = default;

    // Every listing for `vendor_id`, in catalog (ordinal) order, or nullptr if no
    // such vendor exists. The returned pointer is owned by the catalog and valid
    // for its lifetime.
    virtual const std::vector<VendorListing>* listings(std::uint32_t vendor_id) const = 0;

    // The listing for `item_template_id` at `vendor_id`, or nullptr if the vendor
    // does not exist or does not sell that template. Default: a linear scan of
    // listings() (vendor inventories are small); an impl may override.
    virtual const VendorListing* find_listing(std::uint32_t vendor_id,
                                              std::uint32_t item_template_id) const;

    // The server-authoritative BUY price (copper) a `listing` sells for, resolving
    // the override-vs-template-buy_price rule against `templates`. Returns nullopt
    // when the template is unknown OR has no buy price and the listing sets no
    // override (an item that is stocked but not purchasable — treated as NOT_SOLD).
    static std::optional<Copper> buy_price(const VendorListing& listing,
                                           const items::TemplateStore& templates);
};

// Reserved id range for the M1 PLACEHOLDER vendors. Kept distinct from real
// content vendor ids (mcc #28) so a placeholder never masquerades as a compiled
// vendor once #28 lands (mirrors items::kPlaceholderIdBase).
inline constexpr std::uint32_t kPlaceholderVendorIdBase = 990000;

// The single M1 placeholder GENERAL-GOODS vendor id (dev/test convenience).
inline constexpr std::uint32_t kPlaceholderGeneralVendor = kPlaceholderVendorIdBase + 1;

// The M1 placeholder vendor set (ECO-01). A small ORIGINAL, clean-room catalog
// over the placeholder item templates (item_template.cpp) — just enough for buy/
// sell/buyback to have real vendor data before mcc #28 produces the content-
// authored vendors. NOT the content pipeline: the seam's stand-in, dropped at #28.
class PlaceholderVendorCatalog : public VendorCatalog {
public:
    PlaceholderVendorCatalog();
    const std::vector<VendorListing>* listings(std::uint32_t vendor_id) const override;

private:
    std::unordered_map<std::uint32_t, std::vector<VendorListing>> by_vendor_;
};

}  // namespace meridian::vendor
