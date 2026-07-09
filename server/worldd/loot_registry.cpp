// SPDX-License-Identifier: Apache-2.0
//
// worldd — the shared corpse-loot registry (issue #388). See loot_registry.h for
// the design (shared-not-per-connection loot + the M1 corpse-source seam). Clean-
// room, original code (CONTRIBUTING.md).

#include "loot_registry.h"

namespace meridian::worldd {

namespace lo = meridian::loot;

void LootRegistry::insert(lo::LootSession session) {
    const lo::LooterId guid = session.corpse_guid();
    std::lock_guard<std::mutex> lk(mtx_);
    sessions_.insert_or_assign(guid, std::move(session));
}

bool LootRegistry::contains(lo::LooterId corpse_guid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sessions_.find(corpse_guid) != sessions_.end();
}

bool LootRegistry::erase(lo::LooterId corpse_guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    return sessions_.erase(corpse_guid) != 0;
}

std::size_t LootRegistry::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sessions_.size();
}

LootWindow LootRegistry::open(lo::LooterId corpse_guid, lo::LooterId looter,
                              const lo::LootPoint& looter_pos,
                              const lo::QuestPredicate& has_quest) const {
    LootWindow w;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(corpse_guid);
    if (it == sessions_.end()) {
        w.status = LootOpenStatus::kNoSuchCorpse;
        return w;
    }
    const lo::LootSession& s = it->second;

    // Ownership + range pre-checks (mirror the take-time gates so opening a window
    // you cannot loot fails the same way the pull would).
    if (!s.is_owner(looter)) {
        w.status = LootOpenStatus::kNotALooter;
        return w;
    }
    if (lo::loot_distance(looter_pos, s.corpse_pos()) > s.loot_range()) {
        w.status = LootOpenStatus::kOutOfRange;
        return w;
    }

    w.slots = s.visible_slots(looter, has_quest);
    const bool money_available = !s.copper_taken() && s.copper() > 0;
    w.copper = money_available ? s.copper() : items::Copper{0};

    // Owner + in range but there is nothing left to take (shared loot exhausted).
    if (w.slots.empty() && !money_available) {
        w.status = LootOpenStatus::kAlreadyLooted;
        return w;
    }
    w.status = LootOpenStatus::kOk;
    return w;
}

TakeItemOutcome LootRegistry::take_item(lo::LooterId corpse_guid, lo::LooterId looter,
                                        const lo::LootPoint& looter_pos, std::size_t slot,
                                        const lo::QuestPredicate& has_quest,
                                        items::Inventory& inv) {
    TakeItemOutcome out;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(corpse_guid);
    if (it == sessions_.end()) {
        out.status = LootTakeResult::kNoSuchCorpse;
        return out;
    }
    lo::LootSession& s = it->second;

    try {
        out.stack = s.take_item(looter, looter_pos, slot, has_quest, inv);
        out.status = LootTakeResult::kOk;
    } catch (const lo::NotAnOwner&) {
        out.status = LootTakeResult::kNotALooter;
        return out;
    } catch (const lo::LootOutOfRange&) {
        out.status = LootTakeResult::kOutOfRange;
        return out;
    } catch (const lo::LootSlotInvalid&) {
        out.status = LootTakeResult::kInvalidSlot;
        return out;
    } catch (const lo::LootQuestRequired&) {
        out.status = LootTakeResult::kQuestRequired;
        return out;
    } catch (const lo::LootAlreadyTaken&) {
        out.status = LootTakeResult::kAlreadyLooted;
        return out;
    } catch (const items::InventoryFull&) {
        out.status = LootTakeResult::kInventoryFull;
        return out;
    }

    // On a successful shared-loot exhaustion, drop the corpse (it may despawn).
    if (s.fully_looted()) {
        out.fully_looted = true;
        sessions_.erase(it);
    }
    return out;
}

TakeMoneyOutcome LootRegistry::take_money(lo::LooterId corpse_guid, lo::LooterId looter,
                                          const lo::LootPoint& looter_pos) {
    TakeMoneyOutcome out;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(corpse_guid);
    if (it == sessions_.end()) {
        out.status = LootTakeResult::kNoSuchCorpse;
        return out;
    }
    lo::LootSession& s = it->second;

    try {
        out.copper = s.take_money(looter, looter_pos);
        out.status = LootTakeResult::kOk;
    } catch (const lo::NotAnOwner&) {
        out.status = LootTakeResult::kNotALooter;
        return out;
    } catch (const lo::LootOutOfRange&) {
        out.status = LootTakeResult::kOutOfRange;
        return out;
    } catch (const lo::LootAlreadyTaken&) {
        out.status = LootTakeResult::kAlreadyLooted;
        return out;
    }

    if (s.fully_looted()) {
        out.fully_looted = true;
        sessions_.erase(it);
    }
    return out;
}

}  // namespace meridian::worldd
