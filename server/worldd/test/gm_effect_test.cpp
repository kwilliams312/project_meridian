// SPDX-License-Identifier: Apache-2.0
//
// worldd — GM COMMAND SET effect integration test (OPS-02b, #418; epic #21).
//
// CLEAN-ROOM: written from docs/prd/server-prd.md §4-M1 (OPS-02 GM commands) +
// §5.5 (movement authority / forced-move ack barrier), world_state.h,
// movement_validation.h, combat_unit.h. No GPL source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: drives the REAL server-side effect surfaces the GM
// commands act on — WorldState (summon / kick / setlevel) + SessionMovementState
// (the .tele/.summon force-correction + ack barrier) — directly, with CAPTURING
// egress sinks (no TLS, no MariaDB). The command PARSE/GATE/AUDIT half is proven by
// gm_command_test; this proves each command's SERVER STATE CHANGE actually happens:
//   A. .tele/.summon force_correction moves the authoritative position, arms the
//      ack barrier, and clears the speed window — so the client's reconciling move
//      is NOT flagged as a teleport/speed cheat (the #420 anti-cheat interaction).
//   B. .summon moves the TARGET in the world (grid position + AoI relay to
//      observers) and posts the destination to its forced-move mailbox; an unknown
//      name is TargetOffline.
//   C. .kick invokes the target's teardown closure (disconnect signal + AoI leave);
//      an unknown name is TargetOffline; the closure is one-shot.
//   D. .setlevel sets the world-owned Unit level; an unentered slot is a no-op.
//
// The DB-backed half of .additem (mint + persist) rides the same itm mint/place
// path the loot/quest turn-in tests exercise over a live MariaDB; it is not
// re-proven here (this suite is intentionally DB-free, like the movement/chat/AoI
// unit tests).

#include "combat_unit.h"
#include "movement_constants.h"
#include "movement_validation.h"
#include "world_generated.h"
#include "world_state.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

bool approx(float a, float b) { return std::fabs(a - b) < 0.01f; }

// A capturing egress sink that records the entity-guid of every ENTITY_ENTER /
// ENTITY_UPDATE the relay emits to this session (the summon grid-move proof).
struct Capture {
    std::vector<std::uint64_t> entered;
    std::vector<std::uint64_t> updated;

    EgressFn sink() {
        return [this](mn::Opcode op, const std::vector<std::uint8_t>& payload) -> bool {
            if (op == mn::Opcode::ENTITY_ENTER) {
                const auto* e = fb::GetRoot<mn::EntityEnter>(payload.data());
                if (e) entered.push_back(e->entity_guid());
            } else if (op == mn::Opcode::ENTITY_UPDATE) {
                const auto* u = fb::GetRoot<mn::EntityUpdate>(payload.data());
                if (u) updated.push_back(u->entity_guid());
            }
            return true;
        };
    }
    bool saw_enter(std::uint64_t guid) const {
        for (std::uint64_t g : entered) if (g == guid) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// A — .tele/.summon forced move: authoritative reset + ack barrier + no anti-cheat
// ---------------------------------------------------------------------------
void test_forced_move_barrier() {
    std::printf("[gm] .tele/.summon force_correction: move + ack barrier + anti-cheat\n");

    // A mover authoritatively at the play-area centre, one legit intent processed.
    SessionMovementState st(at(-320.0f, -320.0f), /*spawn_time_ms=*/0);
    st.set_entity_guid(7001);

    // GM teleports the mover far across the map (a jump that WOULD trip the R6
    // teleport check on a normal intent). The ack barrier is armed at seq+1.
    const Position dest = at(-364.0f, -284.0f);
    const std::uint32_t ack = st.last_seq() + 1;
    st.force_correction(dest, ack);

    check(".tele: authoritative position reset to the destination",
          approx(st.authoritative().x, -364.0f) && approx(st.authoritative().y, -284.0f));
    check(".tele: forced-move ack barrier armed", st.awaiting_forced_ack());
    check(".tele: ack barrier seq is last+1", st.forced_ack_seq() == ack);
    check(".tele: sliding speed window cleared (jump not counted)",
          st.window_displacement() == 0.0f);

    // A STALE pre-teleport packet (seq < barrier) is frozen out — R9 (no snap-back
    // oracle), NOT a teleport violation.
    {
        MovementIntentPod stale;
        stale.seq = 0;  // predates the ack barrier
        stale.pos = at(-319.5f, -320.0f);
        stale.client_time_ms = 500;
        const MoveDecision d = st.validate_move(stale, stale.client_time_ms);
        check(".tele: a stale pre-teleport intent is frozen (kUnackedForcedMove)",
              !d.accepted && d.reject == MoveReject::kUnackedForcedMove);
    }

    // The client's reconciling move FROM the teleport destination (seq >= barrier,
    // small step) is ACCEPTED — the GM teleport does NOT trip the anti-cheat
    // teleport/speed audit (the whole point of force_correction for #418).
    {
        MovementIntentPod ackd;
        ackd.seq = ack;                  // acknowledges the forced move
        ackd.state_flags =
            static_cast<std::uint32_t>(mc::MoveMode::Run);  // a real locomotion mode
        ackd.pos = at(-363.6f, -284.0f);    // a ~0.4 m step from the destination
        ackd.client_time_ms = 1000;      // ample dt from spawn_time 0
        const MoveDecision d = st.validate_move(ackd, ackd.client_time_ms);
        check(".tele: the reconciling move from the destination is ACCEPTED (no cheat flag)",
              d.accepted && d.reject == MoveReject::kNone);
        st.apply(d, ackd, ackd.client_time_ms);
        check(".tele: the ack disarms the forced-move barrier", !st.awaiting_forced_ack());
    }
}

// ---------------------------------------------------------------------------
// B — .summon moves the target in the world + posts its forced-move mailbox
// ---------------------------------------------------------------------------
void test_summon() {
    std::printf("[gm] .summon: target moved in world + AoI relay + mailbox posted\n");

    WorldState world;
    Capture gm_cap, bob_cap;

    EntityIdentity gm_id;
    gm_id.entity_guid = 5001;
    gm_id.name = "Guildmaster";
    EntityIdentity bob_id;
    bob_id.entity_guid = 5002;
    bob_id.name = "Bob";

    // GM at centre; Bob far away (out of AoI, so no initial enter between them).
    const SessionSlot gm_slot = world.enter(gm_id, at(-320.0f, -320.0f), gm_cap.sink()).slot;
    const SessionSlot bob_slot = world.enter(bob_id, at(-500.0f, -500.0f), bob_cap.sink()).slot;
    (void)gm_slot;
    check(".summon setup: two sessions entered", world.session_count() == 2);
    check(".summon setup: GM does not yet see Bob (far)", !gm_cap.saw_enter(5002));

    // Register Bob's control (the summon mailbox + a no-op disconnect for now).
    ForcedMoveMailbox bob_mailbox;
    world.set_session_control(bob_slot, &bob_mailbox, [] {});

    // A GM summons Bob to the GM's position.
    const Position dest = at(-320.0f, -320.0f);
    const WorldState::TargetOutcome oc =
        world.summon_to("Bob", dest, /*ack_seq=*/0, /*state_flags=*/0, /*server_time_ms=*/1);
    check(".summon: outcome applied", oc == WorldState::TargetOutcome::kApplied);

    // The TARGET was moved authoritatively in the world (its Unit position == dest).
    const Unit* bob_unit = world.unit_for_slot(bob_slot);
    check(".summon: target's world position moved to the summoner",
          bob_unit != nullptr && approx(bob_unit->position().x, -320.0f) &&
              approx(bob_unit->position().y, -320.0f));

    // AoI relay fired: the GM (co-located now) SEES Bob enter its view.
    check(".summon: the summoner now sees the target (AoI EntityEnter relayed)",
          gm_cap.saw_enter(5002));

    // The destination was posted to Bob's forced-move mailbox for his own worker to
    // apply (force_correction + ack barrier), keeping his movement state single-threaded.
    check(".summon: destination posted to the target's forced-move mailbox",
          bob_mailbox.has_pending());
    {
        const std::optional<Position> pending = bob_mailbox.take();
        check(".summon: the mailbox carries the summon destination",
              pending && approx(pending->x, -320.0f) && approx(pending->y, -320.0f));
        check(".summon: taking the mailbox clears it", !bob_mailbox.has_pending());
    }

    // Case-insensitive lookup; an unknown name is offline (no effect).
    check(".summon: case-insensitive name resolves",
          world.summon_to("bob", dest, 0, 0, 2) == WorldState::TargetOutcome::kApplied);
    check(".summon: an unknown name -> TargetOffline",
          world.summon_to("Nobody", dest, 0, 0, 3) ==
              WorldState::TargetOutcome::kTargetOffline);
}

// ---------------------------------------------------------------------------
// C — .kick invokes the target's one-shot teardown closure
// ---------------------------------------------------------------------------
void test_kick() {
    std::printf("[gm] .kick: named session torn down (disconnect + AoI leave)\n");

    WorldState world;
    Capture cap;
    EntityIdentity bob_id;
    bob_id.entity_guid = 6001;
    bob_id.name = "Bob";
    const SessionSlot bob_slot = world.enter(bob_id, at(-320.0f, -320.0f), cap.sink()).slot;

    bool kicked = false;
    // The teardown mirrors the live closure: flip the session's disconnect signal +
    // drop it from the AoI world (the real one also writes Disconnect{KICKED}).
    world.set_session_control(bob_slot, nullptr, [&] {
        kicked = true;
        world.leave(bob_slot);
    });

    check(".kick setup: one session entered", world.session_count() == 1);
    const WorldState::TargetOutcome oc = world.disconnect_by_name("Bob");
    check(".kick: outcome applied", oc == WorldState::TargetOutcome::kApplied);
    check(".kick: the target's teardown closure ran", kicked);
    check(".kick: the target left the AoI world", world.session_count() == 0);

    // Unknown name -> offline; a repeat kick of the now-gone session is offline too
    // (the closure is one-shot and the name index was freed on leave).
    check(".kick: an unknown name -> TargetOffline",
          world.disconnect_by_name("Nobody") == WorldState::TargetOutcome::kTargetOffline);
    check(".kick: kicking the departed session again -> TargetOffline",
          world.disconnect_by_name("Bob") == WorldState::TargetOutcome::kTargetOffline);
}

// ---------------------------------------------------------------------------
// D — .setlevel sets the world-owned Unit level
// ---------------------------------------------------------------------------
void test_setlevel() {
    std::printf("[gm] .setlevel: world Unit level updated\n");

    WorldState world;
    Capture cap;
    EntityIdentity id;
    id.entity_guid = 7001;
    id.name = "Bob";
    const SessionSlot slot = world.enter(id, at(-320.0f, -320.0f), cap.sink()).slot;

    check(".setlevel: applies to an entered session", world.set_unit_level(slot, 42));
    const Unit* u = world.unit_for_slot(slot);
    check(".setlevel: the world Unit level is updated", u != nullptr && u->level() == 42);
    check(".setlevel: an unentered slot is a no-op", !world.set_unit_level(999999, 5));
}

}  // namespace

int main() {
    std::printf("worldd GM command set effect test (OPS-02b #418)\n\n");
    test_forced_move_barrier();
    test_summon();
    test_kick();
    test_setlevel();
    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
