// SPDX-License-Identifier: Apache-2.0
//
// worldd — movement intake + validation v0 UNIT TEST (issue #86, server authority).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §5.5 (the OPS-03 movement
// envelope), docs/movement-spike.md / movement_constants.h (#101 LOCKED constants),
// and decision D-19 (flat bootstrap map). No GPL source consulted (CONTRIBUTING).
//
// PURE / DB-FREE: exercises the validator directly (no DB, no socket, no session,
// no clock — client_time_ms is supplied). It therefore runs in the PLAIN server
// ctest (build.yml `server` job), no MariaDB service needed — unlike #84's grant
// test which is env-guarded. This is the "prefer a pure function so it is DB-free
// unit-testable" ask.
//
// What it proves:
//   A. A LEGAL move is accepted and the authoritative position advances.
//   B. A SPEED HACK (> max × tol over the window) is rejected + snap-back
//      correction to the last authoritative position (the speed-hack proof).
//   C. An OUT-OF-BOUNDS move is rejected + snap-back.
//   D. A within-tolerance JITTER is accepted.
//   E. The SLIDING-WINDOW check rejects a sustained burst that each stays under
//      the per-packet cap (burst-then-idle cheat, SAD §5.5).
//   F. RATE LIMIT: > 10 intents/s are dropped/coalesced; a state change is never
//      throttled.
//   G. Z-vs-ground: a legal jump apex accepted; a z spike beyond ±4 m rejected.
//   H. The GOLDEN CROSS-TRACK FIXTURE (movement_fixture.h) reproduces exactly —
//      the #101 §4 drift trip-wire shared with the client track.

#include "movement_constants.h"
#include "movement_fixture.h"
#include "movement_validation.h"

#include <cstdint>
#include <cstdio>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

// Build an intent at (x,y,z) with a mode + seq + client time.
MovementIntentPod intent_at(float x, float y, float z, mc::MoveMode mode,
                            std::uint32_t seq, std::uint64_t t_ms) {
    MovementIntentPod mi;
    mi.seq = seq;
    mi.state_flags = static_cast<std::uint32_t>(mode);
    mi.pos = {x, y, z, 0.0f};
    mi.client_time_ms = t_ms;
    return mi;
}

// Validate + apply one intent against a session state, returning the decision.
MoveDecision step(SessionMovementState& s, const MovementIntentPod& mi) {
    MoveDecision d = s.validate_move(mi, mi.client_time_ms);
    s.apply(d, mi, mi.client_time_ms);
    return d;
}

}  // namespace

int main() {
    std::printf("worldd movement validation v0 (unit, DB-free)\n");

    // ===== A. LEGAL move accepted, authoritative advances =====================
    {
        // Spawn at (10,10,0) at t=1000. A 0.20 m walk step over 100 ms is inside
        // the walk budget 2.5 × 0.1 × 1.15 = 0.2875 m ⇒ accept + advance.
        SessionMovementState s({10.0f, 10.0f, 0.0f, 0.0f}, 1000);
        MoveDecision d = step(s, intent_at(10.20f, 10.0f, 0.0f, mc::MoveMode::Walk, 1, 1100));
        check("A: legal walk step accepted", d.accepted && d.reject == MoveReject::kNone);
        check("A: authoritative advanced to proposed x",
              approx(s.authoritative().x, 10.20f));
        check("A: ack_seq echoes the intent seq", d.ack_seq == 1);
    }

    // ===== B. SPEED HACK rejected + snap-back (the proof) =====================
    {
        // Spawn at (30,30,0). Propose a 1.50 m run step in 100 ms. Run budget is
        // 6.0 × 0.1 × 1.15 = 0.69 m; 1.50 ≫ 0.69 ⇒ reject (per-packet speed).
        SessionMovementState s({30.0f, 30.0f, 0.0f, 0.0f}, 1000);
        MoveDecision d = step(s, intent_at(31.50f, 30.0f, 0.0f, mc::MoveMode::Run, 7, 1100));
        check("B: speed-hack move rejected", !d.accepted);
        check("B: reject reason is per-packet speed", d.reject == MoveReject::kSpeedPerPacket);
        // Snap-back correction: the MovementState carries the LAST authoritative
        // position (30,30), NOT the client's cheated (31.5,30).
        check("B: correction snaps back to last authoritative x",
              approx(d.pos.x, 30.0f) && approx(d.pos.y, 30.0f));
        check("B: authoritative position UNCHANGED after reject",
              approx(s.authoritative().x, 30.0f) && approx(s.authoritative().y, 30.0f));
        check("B: correction still acks the intent seq (for reconciliation)",
              d.ack_seq == 7);
    }

    // ===== C. OUT-OF-BOUNDS rejected + snap-back ==============================
    {
        // Near the +x edge (128,64). A small 0.5 m step to (128.5,64) passes the
        // speed check (0.5 ≤ 0.69) but lands outside [0,128] ⇒ reject (bounds).
        SessionMovementState s({128.0f, 64.0f, 0.0f, 0.0f}, 1000);
        MoveDecision d = step(s, intent_at(128.5f, 64.0f, 0.0f, mc::MoveMode::Run, 2, 1100));
        check("C: out-of-bounds move rejected", !d.accepted);
        check("C: reject reason is out-of-bounds", d.reject == MoveReject::kOutOfBounds);
        check("C: snap-back to last authoritative (still in bounds)",
              approx(s.authoritative().x, 128.0f));
    }

    // ===== D. Within-tolerance JITTER accepted ================================
    {
        // A tiny 0.05 m wobble (well under any cap) ⇒ accepted, advances.
        SessionMovementState s({64.0f, 64.0f, 0.0f, 0.0f}, 1000);
        MoveDecision d = step(s, intent_at(64.05f, 64.0f, 0.0f, mc::MoveMode::Run, 3, 1100));
        check("D: within-tolerance jitter accepted", d.accepted);
        check("D: authoritative advanced by the jitter", approx(s.authoritative().x, 64.05f));
    }

    // ===== E. SLIDING-WINDOW catches a sustained burst =======================
    {
        // Each step is 0.68 m in 100 ms (right at the per-packet run edge 0.69) —
        // individually legal. But sustained, the 2 s window budget is
        // 6.0 × window_elapsed × 1.15. After enough steps the windowed sum
        // outruns the window budget and the window check trips (burst-then… but
        // here sustained). We walk +x in a straight line at the per-packet edge.
        SessionMovementState s({0.0f, 64.0f, 0.0f, 0.0f}, 1000);
        int rejected_by_window = 0;
        int accepted = 0;
        float x = 0.0f;
        std::uint64_t t = 1000;
        for (std::uint32_t i = 0; i < 30; ++i) {
            t += 100;              // 10/s cadence
            x += 0.68f;            // per-packet-edge step
            if (x > mc::kZoneMaxXY) break;  // stay in bounds so only speed can trip
            MovementIntentPod mi = intent_at(x, 64.0f, 0.0f, mc::MoveMode::Run, 100 + i, t);
            MoveDecision d = step(s, mi);
            if (d.accepted) {
                ++accepted;
            } else if (d.reject == MoveReject::kSpeedWindow) {
                ++rejected_by_window;
                x -= 0.68f;  // snap-back: undo the local x so the next proposed step is legal again
            }
        }
        // The very first few steps are accepted (window budget grows with elapsed
        // time); once the sustained rate exceeds server_speed × window × tol the
        // window check starts rejecting. We just assert BOTH happened.
        check("E: sliding window accepted the early legal steps", accepted > 0);
        check("E: sliding window rejected the sustained over-budget steps",
              rejected_by_window > 0);
    }

    // ===== F. RATE LIMIT: > 10/s dropped; state change never throttled ========
    {
        MovementIntake gate;
        // 5 intents all at the same client time (a > 10/s burst), same flags.
        // The first is admitted; the rest (no time elapsed, no state change) drop.
        int admitted = 0;
        for (std::uint32_t i = 0; i < 5; ++i) {
            MovementIntentPod mi = intent_at(1.0f, 1.0f, 0.0f, mc::MoveMode::Run, i, 2000);
            if (gate.admit(mi, 2000)) ++admitted;
        }
        check("F: only the first of a same-instant burst is admitted", admitted == 1);
        check("F: the rest are dropped/coalesced", gate.dropped() == 4);

        // A STATE CHANGE (Run -> Walk) at the SAME instant is still admitted.
        MovementIntentPod change = intent_at(1.0f, 1.0f, 0.0f, mc::MoveMode::Walk, 99, 2000);
        check("F: a state change is admitted even within the rate window",
              gate.admit(change, 2000));

        // After 100 ms (the 10/s spacing) a non-state-change intent is admitted.
        MovementIntentPod spaced = intent_at(1.0f, 1.0f, 0.0f, mc::MoveMode::Walk, 100, 2100);
        check("F: a properly-spaced intent (>=100ms) is admitted",
              gate.admit(spaced, 2100));
    }

    // ===== G. Z-vs-ground: legal apex accepted, spike rejected ================
    {
        SessionMovementState s({50.0f, 50.0f, 0.0f, 0.0f}, 1000);
        // Legal jump apex z = 0.99 m (inside ±4 m), tiny horizontal ⇒ accept.
        MoveDecision ok = step(s, intent_at(50.10f, 50.0f, 0.99f, mc::MoveMode::Jump, 1, 1100));
        check("G: legal jump apex (z within ±4 m) accepted", ok.accepted);

        // z spike to 5.0 m (> 4 m of flat ground) ⇒ reject (z out of range).
        SessionMovementState s2({40.0f, 40.0f, 0.0f, 0.0f}, 1000);
        MoveDecision bad = step(s2, intent_at(40.10f, 40.0f, 5.0f, mc::MoveMode::Jump, 1, 1100));
        check("G: z spike beyond ±4 m rejected", !bad.accepted);
        check("G: reject reason is z-out-of-range", bad.reject == MoveReject::kZOutOfRange);
        check("G: snap-back keeps last authoritative z", approx(s2.authoritative().z, 0.0f));
    }

    // ===== H. GOLDEN CROSS-TRACK FIXTURE reproduces exactly ===================
    // The #101 §4 drift trip-wire: every pinned vector must yield the exact
    // accept/reject the fixture declares. A shared-constant drift flips one and
    // fails HERE (and, per #101, on the client doctest track too).
    {
        int vec_ok = 0;
        for (const auto& v : fixture::kGoldenVectors) {
            SessionMovementState s(v.start, v.start_time_ms);
            MoveDecision d = s.validate_move(v.intent, v.intent.client_time_ms);
            bool match = (d.accepted == v.expect_accepted) && (d.reject == v.expect_reject);
            check(v.name, match);
            if (match) ++vec_ok;
        }
        check("H: all golden cross-track vectors reproduced",
              vec_ok == static_cast<int>(fixture::kGoldenVectors.size()));
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD MOVEMENT VALIDATION TESTS PASSED\n"
                            : "\n%d WORLDD MOVEMENT VALIDATION TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
