// SPDX-License-Identifier: Apache-2.0
//
// meridian-vendor — the per-session BUYBACK queue (ECO-01; server PRD §4-M1
// "buyback (recently-sold items repurchasable ... for a limited window/queue)";
// issue #370).
//
// When a player sells an item to a vendor, the sold stack is pushed onto this
// bounded queue so it can be repurchased — at the exact copper it was sold for —
// until the queue evicts it. This is the classic vendor "buyback" tab: a small
// ring of the most-recently-sold items, oldest-out when full.
//
// PURE + DETERMINISTIC: no DB, no clock. The "limited window" is expressed as a
// bounded queue (a fixed number of slots) rather than a wall-clock timer, so the
// behaviour is fully deterministic and unit-testable without faking time. The
// queue is SESSION-scoped and NON-durable — a fresh login starts empty (it lives
// on the connection, next to the other per-session state) — matching the classic
// buyback UX and keeping durable economy state to the item/currency tables only.
//
// Clean-room, original code (CONTRIBUTING.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>

#include "item_template.h"  // meridian::items::Copper

namespace meridian::vendor {

using items::Copper;

// Default buyback capacity — the number of recently-sold stacks kept for
// repurchase. Named so it is one place to change; the ctor accepts an override.
inline constexpr std::size_t kDefaultBuybackSlots = 12;

// One repurchasable entry: what was sold, and the copper it was sold for (the
// buyback price is exactly that credit — repurchasing refunds the vendor and
// returns the item). Non-durable; no guid (the original instance was destroyed on
// sell, and buyback mints a fresh one).
struct BuybackEntry {
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;   // units sold as one stack
    Copper price = 0;             // copper credited on sale == copper to repurchase
};

// A slot index was out of range for the current queue contents.
class BuybackSlotEmpty : public std::runtime_error {
public:
    explicit BuybackSlotEmpty(std::size_t slot)
        : std::runtime_error("no buyback entry at slot " + std::to_string(slot)) {}
};

// A bounded FIFO of recently-sold stacks. Slot 0 is the OLDEST entry still held;
// pushing past capacity evicts slot 0. Slot indices are stable only between
// mutations (a push/take renumbers), which matches how the client re-reads the
// buyback tab after each transaction.
class BuybackQueue {
public:
    explicit BuybackQueue(std::size_t capacity = kDefaultBuybackSlots)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // Push a sold stack onto the back of the queue. Returns the slot index it
    // occupies AFTER any eviction (so the caller can tell the client where it
    // landed). When the queue is full the oldest entry (slot 0) is evicted first.
    std::size_t push(const BuybackEntry& entry) {
        if (entries_.size() >= capacity_) entries_.pop_front();
        entries_.push_back(entry);
        return entries_.size() - 1;
    }

    // The entry at `slot`. Throws BuybackSlotEmpty if out of range.
    const BuybackEntry& at(std::size_t slot) const {
        if (slot >= entries_.size()) throw BuybackSlotEmpty(slot);
        return entries_[slot];
    }

    // Remove and return the entry at `slot` (a completed repurchase). Throws
    // BuybackSlotEmpty if out of range. Entries after `slot` shift down by one.
    BuybackEntry take(std::size_t slot) {
        if (slot >= entries_.size()) throw BuybackSlotEmpty(slot);
        BuybackEntry e = entries_[slot];
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(slot));
        return e;
    }

    // Read-only view (client render / tests).
    const std::deque<BuybackEntry>& entries() const { return entries_; }

private:
    std::size_t capacity_;
    std::deque<BuybackEntry> entries_;
};

}  // namespace meridian::vendor
