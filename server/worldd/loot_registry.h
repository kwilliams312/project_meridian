// SPDX-License-Identifier: Apache-2.0
//
// worldd — the shared, THREAD-SAFE corpse-loot registry for the LIVE session path
// (ITM-02 wire wiring; issue #388, epic #20). This is the bridge between the loot
// library (meridian::loot, #369) and the world-dispatch LOOT_* handlers.
//
// WHY A SHARED REGISTRY (not per-connection state): a corpse's loot is SHARED —
// every eligible looter sees + pulls from the SAME loot session (common stacks are
// taken once for everyone; quest stacks are personal, per loot_session.h). So the
// loot sessions cannot live on one connection's ConnCtx (unlike the vendor buyback
// queue, which IS per-session). They live here, keyed by corpse (dead-creature)
// entity guid, guarded by a mutex so IO-worker dispatch threads may open/take/
// release concurrently without racing the world thread that rolls new corpse loot.
//
// SEAM (M1): a creature death rolls its loot table into a loot session on the world
// thread (MapTick::on_unit_died, #369). The world thread hands the rolled session
// to insert() so the live LOOT_* handlers can serve it. At M1 the live combat path
// resolves instants inline against WorldState (world_dispatch CAST_REQUEST) rather
// than feeding MapTick, so no corpse is produced in the plain daemon yet — the
// registry starts empty and the wire-driven corpse SOURCE is the documented seam
// (exactly like the placeholder quest/npc content). The wire-facing dispatch is
// fully live + validated; insert() is the single point the death hook (or a test)
// seeds a corpse.
//
// VALIDATION: every open/take delegates to loot::LootSession's own server-side
// gates (ownership / in-range / not-already-looted / quest-eligibility). Errors are
// mapped here to the wire LootStatus / LootTakeStatus so a handler never inspects a
// loot exception itself. All state mutation happens under the registry mutex; the
// handler performs its DB persistence (mint/place, add_money) OUTSIDE the lock so a
// DB round-trip never stalls another connection's loot access.
//
// Clean-room, original code (CONTRIBUTING.md).

#ifndef MERIDIAN_WORLDD_LOOT_REGISTRY_H
#define MERIDIAN_WORLDD_LOOT_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "inventory.h"      // meridian::items::Inventory (transfer target)
#include "loot_session.h"   // meridian::loot::LootSession / LootSlotView / QuestPredicate

namespace meridian::worldd {

// The wire-facing status of opening a loot window (mirrors world.fbs LootStatus).
enum class LootOpenStatus : std::uint8_t {
    kOk = 0,
    kNotALooter,      // not an eligible looter of the corpse
    kOutOfRange,      // too far from the corpse
    kNoSuchCorpse,    // no loot session by that guid
    kAlreadyLooted,   // the corpse's shared loot is exhausted
};

// The wire-facing status of a take (mirrors world.fbs LootTakeStatus).
enum class LootTakeResult : std::uint8_t {
    kOk = 0,
    kNotALooter,
    kOutOfRange,
    kAlreadyLooted,
    kQuestRequired,
    kInventoryFull,
    kInvalidSlot,
    kNoSuchCorpse,
};

// The result of open(): the status plus (on kOk) the money + the slots THIS looter
// may currently take. `copper` is 0 unless kOk.
struct LootWindow {
    LootOpenStatus                  status = LootOpenStatus::kNoSuchCorpse;
    items::Copper                   copper = 0;
    std::vector<loot::LootSlotView> slots;
};

// The result of take_item(): on kOk `stack` names the moved item (template + count)
// the handler then mints/places durably. On any other status nothing was taken.
struct TakeItemOutcome {
    LootTakeResult  status = LootTakeResult::kNoSuchCorpse;
    loot::LootStack stack;                 // valid only on kOk
    bool            fully_looted = false;  // the corpse's shared loot is now exhausted
};

// The result of take_money(): on kOk `copper` was removed from the corpse.
struct TakeMoneyOutcome {
    LootTakeResult status = LootTakeResult::kNoSuchCorpse;
    items::Copper  copper = 0;
    bool           fully_looted = false;
};

// ---------------------------------------------------------------------------
// LootRegistry — the shared, thread-safe corpse loot store.
// ---------------------------------------------------------------------------
class LootRegistry {
public:
    LootRegistry() = default;
    LootRegistry(const LootRegistry&) = delete;
    LootRegistry& operator=(const LootRegistry&) = delete;

    // Seed / replace the loot session on a corpse (the world-thread death hook, or a
    // test). Any existing session on that guid is overwritten. Thread-safe.
    void insert(loot::LootSession session);

    // Whether a corpse currently has a loot session. Thread-safe.
    bool contains(loot::LooterId corpse_guid) const;

    // Drop a corpse's session (the corpse despawned). Returns true iff one was
    // removed. Thread-safe.
    bool erase(loot::LooterId corpse_guid);

    // How many corpses hold a live session (tests). Thread-safe.
    std::size_t size() const;

    // Open the loot window on `corpse_guid` for `looter` at `looter_pos`. Runs the
    // session's ownership + range pre-checks and returns the slots THIS looter may
    // currently take (visible_slots) + the money. `has_quest` answers the quest gate.
    // Non-mutating. Thread-safe.
    LootWindow open(loot::LooterId corpse_guid, loot::LooterId looter,
                    const loot::LootPoint& looter_pos,
                    const loot::QuestPredicate& has_quest) const;

    // Take slot `slot` of `corpse_guid` into `inv` (which the caller loaded from the
    // DB so the InventoryFull check reflects real capacity). On kOk the slot is
    // marked taken and `stack` names the moved item; the caller then persists it
    // durably. Thread-safe (the whole validate-and-mark step is atomic under the
    // registry lock). `inv` is mutated only on kOk.
    TakeItemOutcome take_item(loot::LooterId corpse_guid, loot::LooterId looter,
                              const loot::LootPoint& looter_pos, std::size_t slot,
                              const loot::QuestPredicate& has_quest, items::Inventory& inv);

    // Take the corpse's shared money pile. On kOk `copper` is the removed amount the
    // caller then credits durably (add_money). Thread-safe.
    TakeMoneyOutcome take_money(loot::LooterId corpse_guid, loot::LooterId looter,
                                const loot::LootPoint& looter_pos);

private:
    mutable std::mutex mtx_;
    // corpse guid -> its loot session (held by value; node stability keeps a
    // session in place across other inserts — callers never hold a reference
    // outside the lock).
    std::unordered_map<loot::LooterId, loot::LootSession> sessions_;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_LOOT_REGISTRY_H
