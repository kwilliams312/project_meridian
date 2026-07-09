// SPDX-License-Identifier: Apache-2.0
//
// worldd — the MapTick→session QUEST-KILL credit bus (issue #396, QST-01; epic
// #20's owner-signed-off event-bus decision).
//
// WHY THIS EXISTS: MapTick is the authority for *what happened in the world* and
// EMITS creature-kill events (map_tick.h TickEventKind::kCreatureKill); the SESSION
// owns the authoritative quest state (world_dispatch.h ConnCtx::quests) and APPLIES
// them. The two live on DIFFERENT threads — MapTick ticks on the world thread, a
// session's quest log is only ever touched by that connection's IO worker. This
// registry is the thread-safe hand-off between them: the world thread PUSHES a kill
// keyed by the killer's guid; the owning session DRAINS its own pending kills at a
// poll point on ITS OWN thread and applies on_kill there. So the quest log stays
// single-threaded (no shared mutable ownership — the #20 decision), and the only
// cross-thread state is this small guid→pending-kills map, fully synchronized here.
//
// GATING: push_kill only enqueues for a guid that is REGISTERED (an in-world
// session tracking quests). A kill whose killer is not a registered session (a
// creature-on-creature death, or a session with no quest log) is dropped — nothing
// accumulates for a guid that will never drain.
//
// CLEAN-ROOM: designed from the epic-#20 event-bus decision + the quest_log.h /
// map_tick.h module headers only. No GPL source consulted (CONTRIBUTING.md).

#ifndef MERIDIAN_WORLDD_QUEST_CREDIT_H
#define MERIDIAN_WORLDD_QUEST_CREDIT_H

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "combat_unit.h"  // ObjectGuid

namespace meridian::worldd {

// A per-registration holder identity. register_session() hands each admission a
// UNIQUE, monotonically-increasing token stored as the guid's current holder;
// unregister_session() is a COMPARE-AND-REMOVE keyed on it. This is the ABA guard
// (mirroring active_sessions.h) that makes a same-guid re-login (single-session
// kick-old on the SAME character) correct: the displaced session's later teardown
// carries a stale token that no longer matches, so it can NEVER unregister the
// session that replaced it.
using QuestCreditToken = std::uint64_t;

// The thread-safe guid→pending-kills registry that bridges MapTick's kill events
// to the owning session's quest log. One per WorldServer, shared across every
// serve_connection (the world thread pushes, IO workers register/drain).
class QuestCreditRegistry {
public:
    QuestCreditRegistry() = default;

    QuestCreditRegistry(const QuestCreditRegistry&) = delete;
    QuestCreditRegistry& operator=(const QuestCreditRegistry&) = delete;

    // Register an in-world session (its entity guid) so kills crediting it are
    // retained until it drains them. Called at ENTER_WORLD, after the quest log is
    // stood up. Returns a fresh holder token the caller passes back to
    // unregister_session(). A repeat register for the same guid (a same-character
    // re-login) issues a NEW token (so the prior holder's unregister no-ops) and
    // keeps any pending kills. Thread-safe.
    QuestCreditToken register_session(ObjectGuid guid);

    // Drop a session (world-leave / disconnect): forget its guid + any undrained
    // kills, but ONLY if `token` still holds the guid (compare-and-remove). A stale
    // token (this session was already displaced by a newer login for the same guid)
    // no-ops. Thread-safe.
    void unregister_session(ObjectGuid guid, QuestCreditToken token);

    // Report a creature kill crediting `killer` (from the world thread draining
    // MapTick's kCreatureKill events). Retained ONLY if `killer` is a registered
    // session; otherwise dropped. Thread-safe.
    void push_kill(ObjectGuid killer, std::uint32_t npc_template_id);

    // Take (and clear) the npc-template ids of the kills pending for `guid`, in the
    // order reported. Empty if the guid is unregistered or has nothing pending. The
    // owning session calls this on its own IO worker, then applies on_kill for each
    // id against its quest log. Thread-safe.
    std::vector<std::uint32_t> drain_kills(ObjectGuid guid);

    // Test/diagnostic: whether `guid` is currently registered.
    bool is_registered(ObjectGuid guid) const;

private:
    struct Entry {
        QuestCreditToken token = 0;             // current holder (ABA guard)
        std::vector<std::uint32_t> kills;       // queued (undrained) kill npc-template ids
    };
    mutable std::mutex mtx_;
    std::unordered_map<ObjectGuid, Entry> pending_;
    QuestCreditToken next_token_ = 1;  // 0 is reserved "no holder"
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_QUEST_CREDIT_H
