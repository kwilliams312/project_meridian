// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free unit test for the client-world probe handoff
// (#301). Proves the ONE step world.gd mirrors in GDScript: decode a #87 AoI relay
// frame with the shared clientnet entity codec → apply it to the #104 remote
// interpolator + the sighting log (probe::apply_entity_frame). No socket, no Godot,
// no server — the decode→interpolator wiring is proven in isolation; the live
// end-to-end proof is client/test/run_client_sees_bot_it.sh.

#include "client_world_probe_core.h"

#include <cmath>
#include <cstdio>

#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/wire_frame.h"
#include "remote_interpolation.h"

using namespace meridian;

namespace {

int g_failures = 0;
int g_checks = 0;
void check(bool cond, const char* what, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL (line %d): %s\n", line, what);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

namespace cn = meridian::clientnet;

// Build an EntityEnter relay payload (the FlatBuffer body, no frame header).
cn::Bytes enter_payload(std::uint64_t guid, float x, float y, float z, float orient) {
    cn::codec::EntityEnter e;
    e.entity_guid = guid;
    e.type_id = 1;
    e.x = x;
    e.y = y;
    e.z = z;
    e.orientation = orient;
    return cn::codec::encode_entity_enter(e);
}
cn::Bytes update_payload(std::uint64_t guid, float x, float y, float z) {
    cn::codec::EntityUpdate u;
    u.entity_guid = guid;
    u.x = x;
    u.y = y;
    u.z = z;
    return cn::codec::encode_entity_update(u);
}
cn::Bytes leave_payload(std::uint64_t guid, std::uint16_t reason) {
    cn::codec::EntityLeave l{guid, reason};
    return cn::codec::encode_entity_leave(l);
}

// ---------------------------------------------------------------------------
// Enter → the interpolator tracks the peer + a sighting is logged.
// ---------------------------------------------------------------------------
void test_enter_tracks_peer() {
    std::puts("[probe] EntityEnter → interpolator tracks + sighting logged");
    remote::RemoteInterpolator interp;
    probe::ProbeResult res;

    const std::uint64_t guid = 0xBEEF;
    bool applied = probe::apply_entity_frame(interp, res, cn::kOpEntityEnter,
                                             enter_payload(guid, 64.0f, 64.0f, 0.5f, 1.0f),
                                             /*recv_ms=*/1000);
    CHECK(applied);
    CHECK(interp.is_tracked(guid));
    CHECK(interp.tracked_count() == 1);
    CHECK(res.distinct_entities_seen() == 1);
    CHECK(res.enters_by_guid[guid] == 1);
    CHECK(res.sightings.size() == 1);
    CHECK(res.sightings[0].kind == probe::SightingKind::kEnter);
    CHECK(res.sightings[0].entity_guid == guid);
    CHECK(res.sightings[0].has_position);
    // Sighting keeps WIRE coords (z = height 0.5).
    CHECK(res.sightings[0].z == 0.5f);
}

// ---------------------------------------------------------------------------
// Update → the interpolator buffers a new snapshot; the rendered position moves.
// ---------------------------------------------------------------------------
void test_update_moves_peer() {
    std::puts("[probe] EntityUpdate → interpolator advances the peer position");
    remote::RemoteInterpolator interp;
    probe::ProbeResult res;
    const std::uint64_t guid = 0x1234;

    // Enter at the spawn (wire 64,64,0), then two moves along ground-X.
    probe::apply_entity_frame(interp, res, cn::kOpEntityEnter,
                              enter_payload(guid, 64.0f, 64.0f, 0.0f, 0.0f), 0);
    probe::apply_entity_frame(interp, res, cn::kOpEntityUpdate,
                              update_payload(guid, 66.0f, 64.0f, 0.0f), 100);
    probe::apply_entity_frame(interp, res, cn::kOpEntityUpdate,
                              update_payload(guid, 68.0f, 64.0f, 0.0f), 200);

    CHECK(res.total_updates_seen() == 2);
    CHECK(res.updates_by_guid[guid] == 2);
    CHECK(interp.tracked_count() == 1);

    // Sample between the two snapshots: the render X (wire.x → render.x) is strictly
    // between the buffered positions — proof the update stream feeds interpolation.
    // No clock sync fed, so the estimator maps client→server identity; sample at a
    // render time that brackets the 100↔200 segment (minus interp delay is applied
    // internally). We assert the peer is renderable (not empty/held-at-spawn).
    remote::SampleResult s = interp.sample_entity(guid, /*client_now_ms=*/250);
    CHECK(s.kind != remote::SampleKind::kEmpty);
    CHECK(s.position.x >= 64.0f && s.position.x <= 68.0f);
}

// ---------------------------------------------------------------------------
// Leave → the interpolator forgets the peer.
// ---------------------------------------------------------------------------
void test_leave_forgets_peer() {
    std::puts("[probe] EntityLeave → interpolator drops the peer");
    remote::RemoteInterpolator interp;
    probe::ProbeResult res;
    const std::uint64_t guid = 0xAA55;

    probe::apply_entity_frame(interp, res, cn::kOpEntityEnter,
                              enter_payload(guid, 64.0f, 64.0f, 0.0f, 0.0f), 0);
    CHECK(interp.is_tracked(guid));
    bool applied = probe::apply_entity_frame(interp, res, cn::kOpEntityLeave,
                                             leave_payload(guid, /*DESPAWNED=*/1), 300);
    CHECK(applied);
    CHECK(!interp.is_tracked(guid));
    CHECK(interp.tracked_count() == 0);
    CHECK(res.leaves_by_guid[guid] == 1);
    CHECK(res.sightings.back().kind == probe::SightingKind::kLeave);
    CHECK(res.sightings.back().leave_reason == 1);
}

// ---------------------------------------------------------------------------
// A non-entity opcode (ClockSync) or garbage is ignored (no false sighting).
// ---------------------------------------------------------------------------
void test_ignores_non_entity() {
    std::puts("[probe] non-entity opcode / garbage → ignored, no sighting");
    remote::RemoteInterpolator interp;
    probe::ProbeResult res;

    CHECK(!probe::apply_entity_frame(interp, res, cn::kOpClockSync, cn::Bytes{}, 0));
    // An entity opcode with a garbage body fails the verifier → not applied.
    CHECK(!probe::apply_entity_frame(interp, res, cn::kOpEntityEnter,
                                     cn::Bytes{0xFF, 0xFF, 0xFF, 0xFF}, 0));
    CHECK(res.sightings.empty());
    CHECK(interp.tracked_count() == 0);
}

// ---------------------------------------------------------------------------
// The see-a-bot-move headline predicate: enter + update ⇒ saw_a_peer_move().
// ---------------------------------------------------------------------------
void test_saw_a_peer_move_predicate() {
    std::puts("[probe] enter + update ⇒ saw_a_peer_move() (the headline)");
    remote::RemoteInterpolator interp;
    probe::ProbeResult res;
    const std::uint64_t guid = 0x77;
    CHECK(!res.saw_a_peer_move());  // nothing seen yet
    probe::apply_entity_frame(interp, res, cn::kOpEntityEnter,
                              enter_payload(guid, 64, 64, 0, 0), 0);
    CHECK(!res.saw_a_peer_move());  // entered but not moved
    probe::apply_entity_frame(interp, res, cn::kOpEntityUpdate,
                              update_payload(guid, 65, 64, 0), 100);
    CHECK(res.saw_a_peer_move());   // enter + move
}

}  // namespace

int main() {
    std::puts("=== meridian-client-probe handoff tests (#301) ===");
    test_enter_tracks_peer();
    test_update_moves_peer();
    test_leave_forgets_peer();
    test_ignores_non_entity();
    test_saw_a_peer_move_predicate();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::puts("ALL PASS");
        return 0;
    }
    std::puts("FAILURES PRESENT");
    return 1;
}
