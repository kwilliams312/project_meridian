// SPDX-License-Identifier: Apache-2.0
//
// worldd — area triggers + POI discovery UNIT TEST (issue #368; WLD-01/03, epic
// #20).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 (area-trigger events +
// OnAreaTrigger hook seam; the map tick evaluating player positions), §4.5 (world
// `area_trigger`/`graveyard` content), area_triggers.h, world_state.h, and
// world.fbs (0x9xxx PoiDiscovered). No GPL source consulted (CONTRIBUTING).
//
// PURE / DB-FREE / SOCKET-FREE / TLS-FREE — runs in the plain server ctest.
// WorldState's egress is a std::function, so the wiring layer is driven with a
// plain lambda that captures the opcodes the relay emits; no TLS/DB needed.
//
// What it proves (the #368 acceptance list, seeded + deterministic):
//   A. MEMBERSHIP: point-in-box `contains` (inside / edge / outside).
//   B. ENTER fires ONCE: crossing into a volume yields one enter event; staying
//      inside yields none.
//   C. LEAVE fires ONCE: crossing out yields one leave event; staying outside none.
//   D. POI DISCOVERY marks + notifies ONCE: first entry into a discovery volume
//      sets discovered_now (and, wired through WorldState, sends POI_DISCOVERED).
//   E. RE-ENTRY DOES NOT RE-FIRE: leaving and re-entering a discovered volume
//      yields an enter event with discovered_now == false, and no second
//      POI_DISCOVERED frame.
//   F. remove() drops per-character bookkeeping.
//   G. WorldState wiring: enter()/on_movement() drive evaluation, route
//      POI_DISCOVERED through the mover's egress, and fire the OnAreaTrigger hook
//      per crossing.
//   H. The M1 placeholder set is well-formed and clear of the (64,64) spawn.

#include "area_triggers.h"
#include "movement_constants.h"
#include "movement_validation.h"  // Position
#include "world_generated.h"
#include "world_state.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;
namespace mn = meridian::net;

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

// A two-volume seeded set: a DISCOVERY POI box [10,20]^2 and a GRAVEYARD box
// [30,40]^2, disjoint, both away from the (64,64) spawn. Volume-load order is the
// event order the core guarantees.
std::vector<TriggerVolume> seeded_set() {
    TriggerVolume d;
    d.id = 1;
    d.kind = TriggerKind::kDiscovery;
    d.area_id = 5;
    d.name_id = 7;
    d.min_x = 10.0f; d.max_x = 20.0f;
    d.min_y = 10.0f; d.max_y = 20.0f;
    d.min_z = mc::kFlatGroundZ - 100.0f; d.max_z = mc::kFlatGroundZ + 100.0f;

    TriggerVolume g;
    g.id = 2;
    g.kind = TriggerKind::kGraveyard;
    g.area_id = 5;
    g.name_id = 8;
    g.min_x = 30.0f; g.max_x = 40.0f;
    g.min_y = 30.0f; g.max_y = 40.0f;
    g.min_z = mc::kFlatGroundZ - 100.0f; g.max_z = mc::kFlatGroundZ + 100.0f;

    return {d, g};
}

// Count enter/leave events of a kind in a batch.
int count_enter(const std::vector<TriggerEvent>& evs) {
    int n = 0;
    for (const auto& e : evs) n += e.entered ? 1 : 0;
    return n;
}
int count_leave(const std::vector<TriggerEvent>& evs) {
    int n = 0;
    for (const auto& e : evs) n += e.entered ? 0 : 1;
    return n;
}

// ---------------------------------------------------------------------------
// (A–F) The pure AreaTriggerSet core.
// ---------------------------------------------------------------------------
void test_core() {
    std::printf("[core] AreaTriggerSet enter/leave/discovery/re-entry\n");
    const AoiId P = 42;

    AreaTriggerSet ts;
    ts.load(seeded_set());
    check("two volumes loaded", ts.volume_count() == 2);

    // A. membership on the raw volume.
    TriggerVolume d = seeded_set()[0];
    check("contains inside", d.contains(at(15, 15)));
    check("contains on edge (inclusive)", d.contains(at(10, 20)));
    check("contains outside", !d.contains(at(9.99f, 15)));

    // Start OUTSIDE everything → no events.
    check("outside start: no events", ts.evaluate(P, at(0, 0)).empty());
    check("not inside D", !ts.is_inside(P, 1));
    check("not discovered D", !ts.is_discovered(P, 1));

    // B + D. Cross INTO the discovery volume → exactly one ENTER, discovered_now.
    {
        auto evs = ts.evaluate(P, at(15, 15));
        check("enter D: one event", evs.size() == 1);
        check("enter D: is an enter", !evs.empty() && evs[0].entered);
        check("enter D: trigger id 1", !evs.empty() && evs[0].trigger_id == 1);
        check("enter D: discovered_now true (first time)",
              !evs.empty() && evs[0].discovered_now);
        check("now inside D", ts.is_inside(P, 1));
        check("now discovered D", ts.is_discovered(P, 1));
    }

    // B (once). Staying INSIDE → no further event (enter does not re-fire).
    check("stay inside D: no event", ts.evaluate(P, at(16, 16)).empty());

    // C + E. Cross OUT of D and INTO G in one move → one LEAVE (D) + one ENTER (G),
    // no discovery (G is not a discovery volume).
    {
        auto evs = ts.evaluate(P, at(35, 35));
        check("move D->G: one leave", count_leave(evs) == 1);
        check("move D->G: one enter", count_enter(evs) == 1);
        bool poi = false;
        for (const auto& e : evs) poi = poi || e.discovered_now;
        check("move D->G: no discovery fired", !poi);
        check("no longer inside D", !ts.is_inside(P, 1));
        check("inside G", ts.is_inside(P, 2));
        check("D still marked discovered", ts.is_discovered(P, 1));
    }

    // E. Return INTO D → an ENTER event, but discovered_now is FALSE (already
    // discovered — re-entry never re-fires discovery). Also LEAVE G.
    {
        auto evs = ts.evaluate(P, at(15, 15));
        check("return to D: one leave (G)", count_leave(evs) == 1);
        check("return to D: one enter (D)", count_enter(evs) == 1);
        bool reenter_discovery = false;
        for (const auto& e : evs)
            if (e.entered && e.trigger_id == 1) reenter_discovery = e.discovered_now;
        check("return to D: discovered_now FALSE (re-entry no re-fire)",
              !reenter_discovery);
    }

    // F. remove() clears bookkeeping: a fresh evaluate at the same spot re-enters.
    ts.remove(P);
    check("after remove: not inside D", !ts.is_inside(P, 1));
    check("after remove: not discovered D", !ts.is_discovered(P, 1));
    {
        auto evs = ts.evaluate(P, at(15, 15));
        check("after remove: re-enter fires enter again", count_enter(evs) == 1);
        bool rediscover = false;
        for (const auto& e : evs)
            if (e.trigger_id == 1) rediscover = e.discovered_now;
        check("after remove: discovery fires again (fresh character state)",
              rediscover);
    }
}

// ---------------------------------------------------------------------------
// (G) WorldState wiring: enter()/on_movement() → POI_DISCOVERED + hook.
// ---------------------------------------------------------------------------
void test_worldstate_wiring() {
    std::printf("[wiring] WorldState drives triggers + emits POI_DISCOVERED\n");

    WorldState ws;
    ws.load_area_triggers(seeded_set());

    // Capture every opcode the relay emits to this (single) session's egress.
    std::vector<std::pair<mn::Opcode, std::vector<std::uint8_t>>> emitted;
    auto egress = [&emitted](mn::Opcode op, const std::vector<std::uint8_t>& payload) {
        emitted.emplace_back(op, payload);
        return true;
    };

    // Record every server-side OnAreaTrigger crossing.
    std::vector<TriggerEvent> hook_events;
    ws.set_area_trigger_hook(
        [&hook_events](AoiId, const TriggerEvent& e) { hook_events.push_back(e); });

    auto poi_count = [&emitted]() {
        int n = 0;
        for (const auto& kv : emitted)
            if (kv.first == mn::Opcode::POI_DISCOVERED) ++n;
        return n;
    };

    EntityIdentity id;
    id.entity_guid = 4242;
    id.type_id = 1;
    id.char_class = 1;

    // Enter OUTSIDE all volumes → no POI, no crossings.
    EnterResult er = ws.enter(id, at(0, 0), egress);
    check("enter outside: no POI_DISCOVERED", poi_count() == 0);
    check("enter outside: no hook crossings", hook_events.empty());

    // Move INTO the discovery volume → exactly one POI_DISCOVERED, correct fields.
    ws.on_movement(er.slot, at(15, 15), /*ack=*/1, /*flags=*/0, /*t=*/100);
    check("move into POI: one POI_DISCOVERED", poi_count() == 1);
    {
        // Decode the POI_DISCOVERED payload (world.fbs 0x9xxx).
        const std::vector<std::uint8_t>* payload = nullptr;
        for (const auto& kv : emitted)
            if (kv.first == mn::Opcode::POI_DISCOVERED) payload = &kv.second;
        bool fields_ok = false;
        if (payload) {
            flatbuffers::Verifier v(payload->data(), payload->size());
            if (v.VerifyBuffer<mn::PoiDiscovered>(nullptr)) {
                const auto* m = flatbuffers::GetRoot<mn::PoiDiscovered>(payload->data());
                fields_ok = m->trigger_id() == 1u && m->area_id() == 5u &&
                            m->name_id() == 7u;
            }
        }
        check("POI_DISCOVERED decodes with expected fields", fields_ok);
    }
    check("hook saw the discovery enter",
          hook_events.size() == 1 && hook_events.back().entered &&
              hook_events.back().kind == TriggerKind::kDiscovery);

    // Move to the graveyard (out of D, into G) → still exactly one POI_DISCOVERED.
    ws.on_movement(er.slot, at(35, 35), 2, 0, 200);
    check("move to graveyard: still one POI_DISCOVERED", poi_count() == 1);

    // Return into the discovery volume → NO second POI_DISCOVERED (re-entry).
    ws.on_movement(er.slot, at(15, 15), 3, 0, 300);
    check("re-entry into POI: NO second POI_DISCOVERED", poi_count() == 1);

    // The hook fired for every crossing (enter D, leave D + enter G, leave G + enter D).
    check("hook fired for all 5 crossings", hook_events.size() == 5);

    // Leaving the world drops bookkeeping (no crash / no leftover).
    ws.leave(er.slot);
    check("after leave: session count 0", ws.session_count() == 0);
}

// ---------------------------------------------------------------------------
// (H) The M1 placeholder set.
// ---------------------------------------------------------------------------
void test_placeholder_set() {
    std::printf("[placeholder] M1 placeholder trigger set\n");
    auto set = placeholder_area_triggers();
    check("placeholder has 3 volumes", set.size() == 3);

    // The (64,64) play-area-centre spawn must NOT sit inside any placeholder volume
    // (a fresh login should not instantly trip a trigger).
    const Position spawn = at(mc::kZoneMaxXY * 0.5f, mc::kZoneMaxXY * 0.5f);
    bool spawn_clear = true;
    for (const auto& v : set) spawn_clear = spawn_clear && !v.contains(spawn);
    check("spawn (64,64) trips no placeholder volume", spawn_clear);

    // Exactly one discovery volume in the placeholder set (the client-facing one).
    int discovery = 0;
    for (const auto& v : set) discovery += (v.kind == TriggerKind::kDiscovery) ? 1 : 0;
    check("placeholder has exactly one discovery POI", discovery == 1);
}

}  // namespace

int main() {
    std::printf("worldd area triggers + POI discovery unit test (WLD-01/03, #368)\n");
    test_core();
    test_worldstate_wiring();
    test_placeholder_set();

    if (g_fail == 0) {
        std::printf("PASS: area triggers + POI discovery\n");
        return 0;
    }
    std::printf("FAIL: %d area-trigger check(s) failed\n", g_fail);
    return 1;
}
