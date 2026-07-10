// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor — the vendor catalog seam + M1 placeholder set (ECO-01; #370).
// See vendor_catalog.h for the seam/placeholder rationale (mcc #28 replaces this).

#include "vendor_catalog.h"

namespace meridian::vendor {

const VendorListing* VendorCatalog::find_listing(
    std::uint32_t vendor_id, std::uint32_t item_template_id) const {
    const std::vector<VendorListing>* list = listings(vendor_id);
    if (list == nullptr) return nullptr;
    for (const VendorListing& l : *list) {
        if (l.item_template_id == item_template_id) return &l;
    }
    return nullptr;
}

std::optional<Copper> VendorCatalog::buy_price(
    const VendorListing& listing, const items::TemplateStore& templates) {
    if (listing.price_override.has_value()) return listing.price_override;
    const items::ItemTemplate* t = templates.find(listing.item_template_id);
    if (t == nullptr) return std::nullopt;
    return t->buy_price;  // nullopt when the template is not purchasable
}

namespace {

VendorListing listing(std::uint32_t template_id,
                      std::optional<Copper> price_override = std::nullopt) {
    VendorListing l;
    l.item_template_id = template_id;
    l.price_override = price_override;
    return l;
}

}  // namespace

PlaceholderVendorCatalog::PlaceholderVendorCatalog() {
    constexpr std::uint32_t B = items::kPlaceholderIdBase;

    // One general-goods vendor selling a representative slice of the placeholder
    // item set: a one-hand weapon, a shield, armor pieces, a stackable consumable
    // and a stackable trade good. Prices come from each template's buy_price
    // (item_template.cpp) EXCEPT Copper Ore, whose template has no buy_price (it is
    // a loot/craft good, not vendor-stocked by default) — so this vendor sets an
    // explicit price_override to demonstrate the override path.
    by_vendor_[kPlaceholderGeneralVendor] = {
        listing(B + 1),               // Worn Shortsword  (buy 100)
        listing(B + 2),               // Cracked Buckler  (buy 75)
        listing(B + 4),               // Rugged Leather Vest (buy 120)
        listing(B + 5),               // Simple Cloth Cap (buy 35)
        listing(B + 7),               // Minor Health Potion (buy 5, stackable)
        listing(B + 8, Copper{6}),    // Copper Ore — no template buy_price; override 6
    };
}

const std::vector<VendorListing>* PlaceholderVendorCatalog::listings(
    std::uint32_t vendor_id) const {
    auto it = by_vendor_.find(vendor_id);
    return it == by_vendor_.end() ? nullptr : &it->second;
}

}  // namespace meridian::vendor
