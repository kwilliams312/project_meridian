// SPDX-License-Identifier: Apache-2.0
//
// worldd — movement intake + validation v0 implementation (issue #86).
// See movement_validation.h for the provenance + the R1..R5 rule statement.

#include "movement_validation.h"

#include <algorithm>
#include <cmath>

namespace meridian::worldd {
namespace mc = meridian::worldd::movement;

namespace {

// Horizontal (x,y) displacement between two positions, in metres. z is validated
// separately (R5) against the ground sample, not as part of the speed budget —
// SAD §5.5's speed check is a GROUND-speed check (server_speed(mode) is a m/s
// horizontal cap), and the jump arc is policed by the ±4 m z-tolerance instead.
float horizontal_distance(const Position& a, const Position& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Elapsed seconds between two client-clock timestamps (ms), with a floor so a
// burst of same-instant packets (Δt = 0) cannot divide the speed budget to zero /
// grant an unbounded per-packet allowance. The floor is one tick (kTickSeconds =
// 50 ms): at M0 intents are ≤ 10/s (100 ms apart), so a Δt below one tick is
// itself anomalous — clamping it to a tick means a same-instant burst is measured
// against a single-tick budget (≈0.345 m at run), which a legit mover never
// exceeds and a teleport-burst cheat does. Backwards/zero client clocks clamp to
// the same floor (never negative, never zero).
double elapsed_seconds(std::uint64_t from_ms, std::uint64_t to_ms) {
    const double raw =
        to_ms > from_ms ? static_cast<double>(to_ms - from_ms) / 1000.0 : 0.0;
    return std::max(raw, mc::kTickSeconds);
}

// The M0 ground height under (x, y). D-19: flat bootstrap map ⇒ a constant plane
// at kFlatGroundZ (= 0). This is the single seam the M1 HeightfieldWorldQuery
// (bilinear over IF-6 chunk data, movement-spike.md §3) drops in behind — the
// validator asks "what is the ground here?" and does not care that the answer is
// currently a constant.
float ground_sample(float /*x*/, float /*y*/) { return mc::kFlatGroundZ; }

// FLAG LEGALITY (R7, OPS-03a #420 / SAD §5.5 "flag legality"). Returns true if the
// state_flags bitfield is an ILLEGAL combination a legit client can never send:
//   • any RESERVED bit set (bits 9..31) — a fabricated flag, e.g. a "fly" hack;
//   • a Swim mode selector while the mover is NOT in a liquid volume — "swim on
//     dry land" (at M0 there is no liquid, so a Swim mode is always illegal);
//   • an UNDEFINED mode selector (values 5..7) — not a real locomotion mode;
//   • CONTRADICTORY direction flags — moving forward AND back, or strafing left
//     AND right, in the same packet (physically impossible).
// The masks are the #102/#247 canonical layout (movement_constants.h §2b).
bool flags_illegal(std::uint32_t f, bool in_liquid) {
    if ((f & mc::kStateFlagsReservedMask) != 0u) return true;  // fabricated / fly bit

    const std::uint32_t mode_sel = f & mc::kStateFlagsModeMask;
    if (mode_sel == mc::kModeSwim && !in_liquid) return true;  // swim on dry land
    if (mode_sel > mc::kModeSwim) return true;                 // undefined mode 5..7

    if ((f & mc::kFlagFwd) && (f & mc::kFlagBack)) return true;         // fwd + back
    if ((f & mc::kFlagStrafeL) && (f & mc::kFlagStrafeR)) return true;  // left + right
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// mode_from_flags
// ---------------------------------------------------------------------------

mc::MoveMode mode_from_flags(std::uint32_t state_flags) {
    // CANONICAL decode (#247): the LOW 3 BITS (kStateFlagsModeMask = 0x7) select the
    // mode (Idle=0/Walk=1/Run=2/Jump=3), matching the MoveMode enum values and the
    // client's encode_state_flags (client movement_controller.cpp #102). The client
    // now stamps the mode into these bits directly — the pre-#247 bot #111
    // wire-boundary workaround is gone. The richer direction/jump/walk flags occupy
    // the bits ABOVE the mode field (movement_constants.h §2b) and do not affect the
    // speed-cap-selecting mode read here. Any value outside the known set falls back
    // to Run — the SAFEST default (largest ground-speed budget, so an unknown flag
    // never spuriously rejects a legal move). The mask + fallback are single-sourced
    // in mode_from_state_flags (movement_constants.h §2b).
    return mc::mode_from_state_flags(state_flags);
}

// ---------------------------------------------------------------------------
// SessionMovementState
// ---------------------------------------------------------------------------

SessionMovementState::SessionMovementState(Position spawn, std::uint64_t spawn_time_ms)
    : authoritative_(spawn), last_time_ms_(spawn_time_ms) {}

void SessionMovementState::prune_window(std::uint64_t now_ms) {
    const auto window_ms =
        static_cast<std::uint64_t>(mc::kSpeedWindowSeconds * 1000.0);
    while (!window_.empty()) {
        const WindowSample& oldest = window_.front();
        // Keep samples whose age is within the window; drop strictly older ones.
        if (now_ms >= oldest.time_ms && (now_ms - oldest.time_ms) > window_ms) {
            window_.pop_front();
        } else {
            break;
        }
    }
}

MoveDecision SessionMovementState::validate_move(const MovementIntentPod& intent,
                                                 std::uint64_t now_ms,
                                                 bool in_liquid) const {
    const mc::MoveMode mode = mode_from_flags(intent.state_flags);
    const float cap = mc::server_speed(mode);

    // A correction (snap-back) MovementState always echoes the intent's seq/flags
    // and holds the LAST AUTHORITATIVE position (the mover reconciles to it).
    auto make_reject = [&](MoveReject why) {
        MoveDecision d;
        d.accepted = false;
        d.reject = why;
        d.ack_seq = intent.seq;
        d.state_flags = intent.state_flags;
        d.pos = authoritative_;  // snap-back
        return d;
    };

    // --- R9. FORCED-MOVE ACK FREEZE (SAD §5.5) ------------------------------
    // While a forced move (GM teleport / knockback / transfer) awaits the client's
    // ack, an intent whose seq predates the ack barrier is a stale pre-teleport
    // packet still in flight — intake is FROZEN until the client's counter catches
    // up. (An intent AT/above the barrier is the acknowledgment — it falls through.)
    if (awaiting_forced_ack_ && intent.seq < forced_ack_seq_) {
        return make_reject(MoveReject::kUnackedForcedMove);
    }

    // --- R8. MONOTONIC SEQUENCE (SAD §5.5 ack counter) ---------------------
    // Within a session the per-client seq must strictly increase; a seq at or below
    // the last processed one is a REPLAY or an OUT-OF-ORDER intent ⇒ reject.
    if (processed_any_ && intent.seq <= last_seq_) {
        return make_reject(MoveReject::kStaleSequence);
    }

    // --- R7. FLAG LEGALITY (SAD §5.5) --------------------------------------
    // Illegal state-flag combination (swim on dry land, fabricated/fly bit,
    // contradictory direction flags) ⇒ reject.
    if (flags_illegal(intent.state_flags, in_liquid)) {
        return make_reject(MoveReject::kIllegalFlag);
    }

    // The Δt this packet is measured over: time since the last processed intent
    // (or since spawn for the first one), floored to one tick.
    const double dt = elapsed_seconds(last_time_ms_, now_ms);
    const float step = horizontal_distance(authoritative_, intent.pos);

    // --- R6. TELEPORT (SAD §5.5 "displacement > window budget = hard violation") -
    // A single-packet horizontal jump beyond a full window of running at the cap is
    // a warp/blink, not a fast move. Checked BEFORE the graduated speed checks so a
    // teleport is named its own kind (kTeleportHardBudget = run × 2 s × 1.15).
    if (step > mc::kTeleportHardBudget) {
        return make_reject(MoveReject::kTeleport);
    }

    // --- R2. PER-PACKET SPEED (SAD §5.5) -----------------------------------
    // displacement ≤ server_speed(mode) × Δt × 1.15.
    const float per_packet_budget =
        cap * static_cast<float>(dt) * mc::kSpeedTolerance;
    if (step > per_packet_budget) {
        return make_reject(MoveReject::kSpeedPerPacket);
    }

    // --- R3. SLIDING-WINDOW SPEED (SAD §5.5) --------------------------------
    // Sum of accepted displacements over the trailing 2 s (INCLUDING this
    // proposed step) ≤ server_speed(mode) × window_elapsed × 1.15. window_elapsed
    // is how much of the 2 s window actually has samples (so early in a session,
    // before 2 s of history exists, the budget is the elapsed time, not a free
    // full-window allowance). We evaluate on a pruned copy of the window without
    // mutating state (validate is const; apply commits).
    {
        const auto window_ms =
            static_cast<std::uint64_t>(mc::kSpeedWindowSeconds * 1000.0);
        float windowed = step;  // this proposed step counts toward the sum
        // The window budget's denominator is the elapsed time the accumulated
        // displacement covers. The CURRENT step occupies the interval
        // [last_time_ms_, now_ms], so the window effectively starts no later than
        // the previous processed time — otherwise the very first packet (empty
        // window) would be judged against a single-tick budget instead of its own
        // Δt, spuriously rejecting a legal first move. Seed the earliest-time from
        // last_time_ms_ and pull it back further for any retained older sample.
        std::uint64_t oldest_ms =
            last_time_ms_ <= now_ms ? last_time_ms_ : now_ms;
        for (auto it = window_.rbegin(); it != window_.rend(); ++it) {
            if (now_ms >= it->time_ms && (now_ms - it->time_ms) <= window_ms) {
                windowed += it->displacement;
                oldest_ms = std::min(oldest_ms, it->time_ms);
            }
        }
        // Clamp the window span to at most the configured window so a long idle
        // gap does not inflate the sustained-speed budget.
        if (now_ms - oldest_ms > window_ms) oldest_ms = now_ms - window_ms;
        const double window_elapsed =
            std::max(elapsed_seconds(oldest_ms, now_ms), mc::kTickSeconds);
        const float window_budget =
            cap * static_cast<float>(window_elapsed) * mc::kSpeedTolerance;
        if (windowed > window_budget) {
            return make_reject(MoveReject::kSpeedWindow);
        }
    }

    // --- R4. MAP BOUNDS (SAD §5.5 "inside map bounds"; D-19 bootstrap area) -
    if (intent.pos.x < mc::kZoneMinXY || intent.pos.x > mc::kZoneMaxXY ||
        intent.pos.y < mc::kZoneMinXY || intent.pos.y > mc::kZoneMaxXY) {
        return make_reject(MoveReject::kOutOfBounds);
    }

    // --- R5. Z-vs-GROUND (SAD §5.5 "z within ±4 m of heightfield sample") ---
    const float ground = ground_sample(intent.pos.x, intent.pos.y);
    if (std::fabs(intent.pos.z - ground) > mc::kHeightTolerance) {
        return make_reject(MoveReject::kZOutOfRange);
    }

    // --- ACCEPTED: authoritative advance. ----------------------------------
    MoveDecision d;
    d.accepted = true;
    d.reject = MoveReject::kNone;
    d.ack_seq = intent.seq;
    d.state_flags = intent.state_flags;
    d.pos = intent.pos;  // the server adopts the validated proposed position
    return d;
}

void SessionMovementState::apply(const MoveDecision& decision,
                                 const MovementIntentPod& intent,
                                 std::uint64_t now_ms) {
    // A REPLAYED / OUT-OF-ORDER intent (R8) or a stale pre-forced-move packet (R9)
    // is DISCARDED as if never seen: the authoritative cursor (seq/time/window) must
    // NOT move — otherwise a replay could drag last_seq_ backward or reset the Δt
    // clock and reopen the speed budget. It already produced its snap-back decision.
    if (decision.reject == MoveReject::kStaleSequence ||
        decision.reject == MoveReject::kUnackedForcedMove) {
        return;
    }

    // Prune first so the window reflects "as of now" before we add this sample.
    prune_window(now_ms);

    if (decision.accepted) {
        const float step = horizontal_distance(authoritative_, decision.pos);
        authoritative_ = decision.pos;
        window_.push_back(WindowSample{now_ms, step});
    }
    // Rejected (speed/bounds/z/flag/teleport): authoritative_ UNCHANGED (snap-back).
    // No window sample (a rejected move must not contribute to the speed budget).

    // The intent WAS processed: record it so the next Δt / monotonic / rate check
    // measures from here.
    processed_any_ = true;
    last_seq_ = intent.seq;
    last_flags_ = intent.state_flags;
    last_time_ms_ = now_ms;

    // A forced-move ack barrier is cleared once the client's counter reaches it —
    // this intent's seq is >= the barrier (else R9 would have discarded it above),
    // so processing it here is the acknowledgment that un-freezes intake.
    if (awaiting_forced_ack_ && intent.seq >= forced_ack_seq_) {
        awaiting_forced_ack_ = false;
    }
}

void SessionMovementState::force_correction(const Position& corrected,
                                            std::uint32_t ack_seq) {
    // The server authoritatively relocates the mover (GM teleport / knockback /
    // transfer placement) and ARMS the ack barrier: intents predating `ack_seq` are
    // frozen until the client's counter reaches it (R9). The sliding speed window is
    // CLEARED — the discontinuous jump must not count against the sustained-speed
    // budget of the mover's next legit step (SAD §5.5 "a shard transfer resets the
    // sliding window").
    authoritative_ = corrected;
    awaiting_forced_ack_ = true;
    forced_ack_seq_ = ack_seq;
    window_.clear();
}

float SessionMovementState::window_displacement() const {
    float sum = 0.0f;
    for (const WindowSample& s : window_) sum += s.displacement;
    return sum;
}

// ---------------------------------------------------------------------------
// MovementIntake (R1 rate class)
// ---------------------------------------------------------------------------

bool MovementIntake::admit(const MovementIntentPod& intent, std::uint64_t now_ms) {
    // First intent for the session is always admitted.
    if (!have_admitted_) {
        have_admitted_ = true;
        last_flags_ = intent.state_flags;
        last_admit_ms_ = now_ms;
        ++admitted_;
        return true;
    }

    // A STATE CHANGE (mode toggle) is always admitted — never throttled away
    // (SAD §5.5 "≤ 10/s/client + state changes").
    const bool state_change = intent.state_flags != last_flags_;

    // Otherwise admit only if at least the min spacing has elapsed. A backwards /
    // equal client clock is treated as "no time elapsed" ⇒ dropped (non-state).
    const bool enough_elapsed =
        now_ms > last_admit_ms_ && (now_ms - last_admit_ms_) >= kMinIntentSpacingMs;

    if (state_change || enough_elapsed) {
        last_flags_ = intent.state_flags;
        last_admit_ms_ = now_ms;
        ++admitted_;
        return true;
    }

    ++dropped_;
    return false;
}

}  // namespace meridian::worldd
