// SPDX-License-Identifier: Apache-2.0
//
// worldd — the MapTick→session VITALS egress bus (issue #437, UI-01 HUD; part of
// epic #24). The sibling of the QST-01 quest-credit bus (quest_credit.h, #396):
// same MapTick↔session seam, a different payload.
//
// WHY THIS EXISTS: MapTick is the authority for what happens in the world and drives
// a player's XP / LEVEL-UP (map_tick.cpp award_kill_xp, CHR-03 #360) — but MapTick is
// socket-free, so it cannot push the resulting VITALS_UPDATE itself. The SESSION owns
// the client egress + its WorldState unit, and both live on a DIFFERENT thread (the
// map ticks on the world thread; a session's unit is only ever mutated by that
// connection's IO worker — the M1 per-connection combat model). This registry is the
// thread-safe hand-off: the world thread PUSHES the level-up's new authoritative
// vitals keyed by the subject's guid; the owning session DRAINS them on ITS OWN
// worker and mirrors them onto its WorldState unit + broadcasts (poll_vitals_egress).
// So the session's unit stays single-threaded and the only cross-thread state is this
// small guid→pending-snapshot map, fully synchronized here.
//
// COALESCING: vitals are ABSOLUTE state (not incremental like a quest kill count), so
// a guid retains only the LATEST pending snapshot — two level-ups before a drain
// collapse to the final one (the client only needs the current values). push_vitals
// enqueues ONLY for a REGISTERED (in-world) session; a snapshot for an unregistered
// guid is dropped (nothing accumulates for a guid that will never drain).
//
// The register/unregister ABA-token guard is identical to quest_credit.h: a same-guid
// re-login (single-session kick-old) issues a fresh token so the displaced session's
// later unregister can never remove the session that replaced it.
//
// CLEAN-ROOM: designed from the epic-#20 event-bus decision + the quest_credit.h /
// map_tick.h module headers only. No GPL source consulted (CONTRIBUTING.md).

#ifndef MERIDIAN_WORLDD_VITALS_EGRESS_H
#define MERIDIAN_WORLDD_VITALS_EGRESS_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "combat_unit.h"  // ObjectGuid
#include "map_tick.h"      // VitalsSnapshot

namespace meridian::worldd {

// A per-registration holder identity (the ABA guard; see quest_credit.h's
// QuestCreditToken).
using VitalsEgressToken = std::uint64_t;

// The thread-safe guid→pending-vitals registry that bridges MapTick's vitals changes
// (level-ups) to the owning session's client egress. One per WorldServer, shared
// across every serve_connection (the world thread pushes, IO workers register/drain).
class VitalsEgressRegistry {
public:
    VitalsEgressRegistry() = default;

    VitalsEgressRegistry(const VitalsEgressRegistry&) = delete;
    VitalsEgressRegistry& operator=(const VitalsEgressRegistry&) = delete;

    // Register an in-world session (its entity guid) so vitals changes crediting it
    // are retained until it drains them. Called at ENTER_WORLD. Returns a fresh holder
    // token the caller passes back to unregister_session(). A repeat register for the
    // same guid (a same-character re-login) issues a NEW token (so the prior holder's
    // unregister no-ops) and keeps any pending snapshot. Thread-safe.
    VitalsEgressToken register_session(ObjectGuid guid);

    // Drop a session (world-leave / disconnect): forget its guid + any undrained
    // snapshot, but ONLY if `token` still holds the guid (compare-and-remove). A stale
    // token (this session was already displaced by a newer login for the same guid)
    // no-ops. Thread-safe.
    void unregister_session(ObjectGuid guid, VitalsEgressToken token);

    // Report a player's post-change authoritative vitals (from the world thread
    // draining MapTick's kVitalsChanged events). Retained (COALESCING onto the latest)
    // ONLY if `snap.guid` is a registered session; otherwise dropped. Thread-safe.
    void push_vitals(const VitalsSnapshot& snap);

    // Take (and clear) the latest pending snapshot for `guid`, or std::nullopt if the
    // guid is unregistered or has nothing pending. The owning session calls this on its
    // own IO worker, then mirrors the snapshot onto its WorldState unit + broadcasts.
    // Thread-safe.
    std::optional<VitalsSnapshot> drain_vitals(ObjectGuid guid);

    // Test/diagnostic: whether `guid` is currently registered.
    bool is_registered(ObjectGuid guid) const;

private:
    struct Entry {
        VitalsEgressToken token = 0;             // current holder (ABA guard)
        std::optional<VitalsSnapshot> pending;   // latest (coalesced) undrained snapshot
    };
    mutable std::mutex mtx_;
    std::unordered_map<ObjectGuid, Entry> pending_;
    VitalsEgressToken next_token_ = 1;  // 0 is reserved "no holder"
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_VITALS_EGRESS_H
