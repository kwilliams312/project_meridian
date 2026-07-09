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

#include "movement_audit.h"       // move_reject_reason (v1 envelope, #420)
#include "movement_constants.h"
#include "movement_fixture.h"
#include "movement_validation.h"
#include "world_metrics.h"        // move_reject_kind (v1 envelope, #420)

#include <cstdint>
#include <cstdio>
#include <cstring>

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

    // ===== I. CANONICAL state_flags: client encoding decodes to the right mode =
    // (#247) The client now encodes the active MoveMode into the LOW 3 BITS with
    // direction/jump/walk flags ABOVE (movement_constants.h §2b). This proves the
    // server decodes the CLIENT'S REAL encoding — no wire-boundary workaround — via
    // the shared canonical mirror (movement_fixture.h client_encode_state_flags).
    {
        // (I.1) Every mode + direction combo the client can emit decodes back to the
        //       SAME mode on the server. The direction/jump/walk flags set ABOVE the
        //       mode bits must NOT change the decoded mode.
        struct Combo { mc::MoveMode mode; float mx; float mz; bool jump; bool walk; };
        const Combo combos[] = {
            {mc::MoveMode::Idle, 0.0f, 0.0f, false, false},
            {mc::MoveMode::Run,  0.0f, 1.0f, false, false},   // forward run
            {mc::MoveMode::Run,  0.0f, -1.0f, false, false},  // backpedal
            {mc::MoveMode::Walk, 1.0f, 0.0f, false, true},    // strafe-right walk
            {mc::MoveMode::Run,  -1.0f, 1.0f, false, false},  // fwd + strafe-left
            {mc::MoveMode::Jump, 0.0f, 1.0f, true, false},    // forward jump
        };
        int rt_ok = 0;
        const int n = static_cast<int>(sizeof(combos) / sizeof(combos[0]));
        for (const Combo& c : combos) {
            const std::uint32_t enc =
                fixture::client_encode_state_flags(c.mode, c.mx, c.mz, c.jump, c.walk);
            if (mode_from_flags(enc) == c.mode) ++rt_ok;
        }
        check("I: client-encoded flags decode to the same mode (all combos)", rt_ok == n);

        // (I.2) A canonically-encoded RUN move is accepted at the RUN speed cap. A
        //       forward-run step of 0.68 m in 100 ms is inside the run budget
        //       (0.69 m) but OVER the walk budget (0.2875 m): it is accepted ONLY
        //       because the server decodes Run from the client's low-3-bit mode. If
        //       the low bits were misread (pre-#247: forward=1 => Walk), the SAME
        //       step would be rejected — so this move being accepted is the fix's
        //       positive proof, using the client's real encoding.
        {
            const std::uint32_t run_flags = fixture::client_encode_state_flags(
                mc::MoveMode::Run, /*mx=*/0.0f, /*mz=*/1.0f, /*jump=*/false, /*walk=*/false);
            check("I: run flags decode as Run (not Walk)",
                  mode_from_flags(run_flags) == mc::MoveMode::Run);
            SessionMovementState s({20.0f, 20.0f, 0.0f, 0.0f}, 1000);
            MovementIntentPod mi;
            mi.seq = 1;
            mi.state_flags = run_flags;
            mi.pos = {20.68f, 20.0f, 0.0f, 0.0f};   // 0.68 m: legal at run, illegal at walk
            mi.client_time_ms = 1100;
            MoveDecision d = step(s, mi);
            check("I: canonical run move accepted at the run cap", d.accepted);
            check("I: authoritative advanced to the run step",
                  approx(s.authoritative().x, 20.68f));
        }

        // (I.3) A SPEED HACK using the client's real RUN encoding is still rejected:
        //       1.50 m in 100 ms ≫ 0.69 m run budget. Proves the fix does not weaken
        //       the speed check (the decoded mode gives the RUN cap, and the hack
        //       still overruns it) — the speed-hack proof on the canonical encoding.
        {
            const std::uint32_t run_flags = fixture::client_encode_state_flags(
                mc::MoveMode::Run, 0.0f, 1.0f, false, false);
            SessionMovementState s({30.0f, 30.0f, 0.0f, 0.0f}, 1000);
            MovementIntentPod mi;
            mi.seq = 2;
            mi.state_flags = run_flags;
            mi.pos = {31.50f, 30.0f, 0.0f, 0.0f};   // 1.50 m ≫ run budget
            mi.client_time_ms = 1100;
            MoveDecision d = step(s, mi);
            check("I: speed-hack on canonical run encoding rejected", !d.accepted);
            check("I: reject reason is per-packet speed",
                  d.reject == MoveReject::kSpeedPerPacket);
            check("I: snap-back keeps last authoritative x",
                  approx(s.authoritative().x, 30.0f));
        }
    }

    // ========================================================================
    // ===== v1 FULL ENVELOPE (OPS-03a, #420) =================================
    // ========================================================================

    // ===== J. TELEPORT: a single-packet warp beyond the hard budget rejected ==
    {
        // Spawn at (10,10). A 20 m jump in 100 ms is > kTeleportHardBudget (13.8 m):
        // classified as a TELEPORT (its own kind), not merely a speed over-cap. The
        // target (30,10) is IN bounds so only the teleport check can trip it.
        SessionMovementState s({10.0f, 10.0f, 0.0f, 0.0f}, 1000);
        MoveDecision d = step(s, intent_at(30.0f, 10.0f, 0.0f, mc::MoveMode::Run, 1, 1100));
        check("J: teleport (warp) move rejected", !d.accepted);
        check("J: reject reason is teleport", d.reject == MoveReject::kTeleport);
        check("J: snap-back to last authoritative position",
              approx(s.authoritative().x, 10.0f) && approx(s.authoritative().y, 10.0f));
        check("J: teleport budget is a full run-window (13.8 m)",
              approx(mc::kTeleportHardBudget, 13.8f, 1e-3f));
    }

    // ===== K. FLAG LEGALITY: swim-on-dry-land / fly bit / contradictory dir ====
    {
        // K.1 Swim mode (selector value 4) while NOT in liquid = swim on dry land.
        SessionMovementState s({20.0f, 20.0f, 0.0f, 0.0f}, 1000);
        MovementIntentPod swim = intent_at(20.05f, 20.0f, 0.0f, mc::MoveMode::Run, 1, 1100);
        swim.state_flags = mc::kModeSwim;  // low-3-bits = Swim
        MoveDecision d = s.validate_move(swim, swim.client_time_ms, /*in_liquid=*/false);
        s.apply(d, swim, swim.client_time_ms);
        check("K.1: swim flag on dry land rejected", !d.accepted);
        check("K.1: reject reason is illegal flag", d.reject == MoveReject::kIllegalFlag);
        check("K.1: snap-back keeps last authoritative x", approx(s.authoritative().x, 20.0f));

        // K.2 The SAME swim flag IS legal when the mover is in a liquid volume
        //     (the M1 water seam): a small step is then accepted (no flag reject).
        SessionMovementState s2({20.0f, 20.0f, 0.0f, 0.0f}, 1000);
        MovementIntentPod swim2 = intent_at(20.05f, 20.0f, 0.0f, mc::MoveMode::Run, 1, 1100);
        swim2.state_flags = mc::kModeSwim;
        MoveDecision d2 = s2.validate_move(swim2, swim2.client_time_ms, /*in_liquid=*/true);
        check("K.2: swim flag IN a liquid volume is not an illegal flag", d2.accepted);

        // K.3 A fabricated RESERVED bit (a "fly" hack, bit 9+) is illegal.
        SessionMovementState s3({30.0f, 30.0f, 0.0f, 0.0f}, 1000);
        MovementIntentPod fly = intent_at(30.05f, 30.0f, 0.0f, mc::MoveMode::Run, 1, 1100);
        fly.state_flags = static_cast<std::uint32_t>(mc::MoveMode::Run) | (1u << 9);
        MoveDecision d3 = step(s3, fly);
        check("K.3: fabricated reserved/fly bit rejected", !d3.accepted);
        check("K.3: reject reason is illegal flag", d3.reject == MoveReject::kIllegalFlag);

        // K.4 CONTRADICTORY direction flags (forward AND back at once) are illegal.
        SessionMovementState s4({40.0f, 40.0f, 0.0f, 0.0f}, 1000);
        MovementIntentPod contra = intent_at(40.05f, 40.0f, 0.0f, mc::MoveMode::Run, 1, 1100);
        contra.state_flags =
            static_cast<std::uint32_t>(mc::MoveMode::Run) | mc::kFlagFwd | mc::kFlagBack;
        MoveDecision d4 = step(s4, contra);
        check("K.4: contradictory fwd+back direction flags rejected", !d4.accepted);
        check("K.4: reject reason is illegal flag", d4.reject == MoveReject::kIllegalFlag);

        // K.5 A legal canonical forward-run (fwd bit set, no contradiction) is NOT
        //     flagged — the direction bits above the mode are legitimate.
        SessionMovementState s5({50.0f, 50.0f, 0.0f, 0.0f}, 1000);
        MovementIntentPod fwd = intent_at(50.05f, 50.0f, 0.0f, mc::MoveMode::Run, 1, 1100);
        fwd.state_flags = static_cast<std::uint32_t>(mc::MoveMode::Run) | mc::kFlagFwd;
        MoveDecision d5 = step(s5, fwd);
        check("K.5: legal forward-run direction flag accepted", d5.accepted);
    }

    // ===== L. MONOTONIC SEQUENCE: replay + out-of-order rejected ==============
    {
        SessionMovementState s({60.0f, 60.0f, 0.0f, 0.0f}, 1000);
        // First legal step at seq=5.
        MoveDecision first = step(s, intent_at(60.10f, 60.0f, 0.0f, mc::MoveMode::Run, 5, 1100));
        check("L: first move (seq=5) accepted", first.accepted);
        // REPLAY: the SAME seq=5 again is a duplicate ⇒ reject (stale sequence).
        MoveDecision replay = step(s, intent_at(60.20f, 60.0f, 0.0f, mc::MoveMode::Run, 5, 1200));
        check("L: replayed seq=5 rejected", !replay.accepted);
        check("L: replay reject reason is stale sequence",
              replay.reject == MoveReject::kStaleSequence);
        // A replay is DISCARDED: the authoritative cursor did NOT move (still 60.10).
        check("L: replay leaves authoritative position untouched",
              approx(s.authoritative().x, 60.10f));
        check("L: replay does not drag last_seq backward", s.last_seq() == 5);
        // OUT OF ORDER: seq=3 (< last processed 5) ⇒ reject (stale sequence).
        MoveDecision ooo = step(s, intent_at(60.15f, 60.0f, 0.0f, mc::MoveMode::Run, 3, 1300));
        check("L: out-of-order seq=3 rejected", !ooo.accepted);
        check("L: out-of-order reject reason is stale sequence",
              ooo.reject == MoveReject::kStaleSequence);
        // A strictly-greater seq=6 resumes normally.
        MoveDecision next = step(s, intent_at(60.20f, 60.0f, 0.0f, mc::MoveMode::Run, 6, 1400));
        check("L: strictly-increasing seq=6 accepted", next.accepted);
    }

    // ===== M. FORCED-MOVE ACK FREEZE (GM teleport / knockback) ================
    {
        SessionMovementState s({64.0f, 64.0f, 0.0f, 0.0f}, 1000);
        MoveDecision m0 = step(s, intent_at(64.10f, 64.0f, 0.0f, mc::MoveMode::Run, 2, 1100));
        check("M: pre-force legal move accepted", m0.accepted);

        // The server forces the mover to (100,100) and demands the client's counter
        // reach ack=5 before its intents resume (un-acked counters freeze intake).
        s.force_correction({100.0f, 100.0f, 0.0f, 0.0f}, /*ack_seq=*/5);
        check("M: forced move relocated the authoritative position",
              approx(s.authoritative().x, 100.0f) && approx(s.authoritative().y, 100.0f));
        check("M: intake is now awaiting the forced-move ack", s.awaiting_forced_ack());

        // A STALE pre-teleport intent (seq=3 < 5) is frozen — rejected, discarded,
        // and it does NOT advance from the client's stale pre-teleport position.
        MoveDecision stale =
            step(s, intent_at(64.20f, 64.0f, 0.0f, mc::MoveMode::Run, 3, 1200));
        check("M: stale pre-forced-move intent frozen", !stale.accepted);
        check("M: freeze reject reason is unacked forced move",
              stale.reject == MoveReject::kUnackedForcedMove);
        check("M: frozen intent snaps back to the FORCED position (100,100)",
              approx(stale.pos.x, 100.0f) && approx(stale.pos.y, 100.0f));
        check("M: still awaiting ack after a stale intent", s.awaiting_forced_ack());

        // The client acknowledges by reaching seq=5 with a legal step from (100,100).
        MoveDecision ack =
            step(s, intent_at(100.10f, 100.0f, 0.0f, mc::MoveMode::Run, 5, 1300));
        check("M: acknowledging intent (seq>=barrier) accepted", ack.accepted);
        check("M: intake un-frozen after the ack", !s.awaiting_forced_ack());
        check("M: authoritative advanced from the forced position",
              approx(s.authoritative().x, 100.10f));
    }

    // ===== N. AUDIT + METRIC vocabularies for each reject kind ================
    // The dispatch maps every reject to a fine-grained audit `reason` and a coarse
    // metric `kind`; pin both so a taxonomy change is caught here.
    {
        check("N: reason speed_per_packet",
              std::strcmp(move_reject_reason(MoveReject::kSpeedPerPacket),
                          "speed_per_packet") == 0);
        check("N: reason teleport",
              std::strcmp(move_reject_reason(MoveReject::kTeleport), "teleport") == 0);
        check("N: reason illegal_flag",
              std::strcmp(move_reject_reason(MoveReject::kIllegalFlag),
                          "illegal_flag") == 0);
        check("N: reason stale_sequence",
              std::strcmp(move_reject_reason(MoveReject::kStaleSequence),
                          "stale_sequence") == 0);
        check("N: reason unacked_forced_move",
              std::strcmp(move_reject_reason(MoveReject::kUnackedForcedMove),
                          "unacked_forced_move") == 0);
        check("N: metric kind flag for illegal flag",
              move_reject_kind(MoveReject::kIllegalFlag) == "flag");
        check("N: metric kind teleport for a warp",
              move_reject_kind(MoveReject::kTeleport) == "teleport");
        check("N: metric kind speed for window over-cap",
              move_reject_kind(MoveReject::kSpeedWindow) == "speed");
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD MOVEMENT VALIDATION TESTS PASSED\n"
                            : "\n%d WORLDD MOVEMENT VALIDATION TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
