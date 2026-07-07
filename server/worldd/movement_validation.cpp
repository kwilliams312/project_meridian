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

}  // namespace

// ---------------------------------------------------------------------------
// mode_from_flags
// ---------------------------------------------------------------------------

mc::MoveMode mode_from_flags(std::uint32_t state_flags) {
    // v0 minimal decode: the low 3 bits select the mode (walk=1/run=2/jump=3),
    // matching the MoveMode enum values (movement_constants.h §2). A richer
    // per-bit forward/back/strafe/jump/swim layout is a #102/#101 follow-up; v0
    // needs only the speed-cap-selecting active mode. Any value outside the known
    // set falls back to Run — the SAFEST default (largest ground-speed budget, so
    // an unknown flag never spuriously rejects a legal move).
    switch (state_flags & 0x7u) {
        case static_cast<std::uint32_t>(mc::MoveMode::Idle): return mc::MoveMode::Idle;
        case static_cast<std::uint32_t>(mc::MoveMode::Walk): return mc::MoveMode::Walk;
        case static_cast<std::uint32_t>(mc::MoveMode::Run):  return mc::MoveMode::Run;
        case static_cast<std::uint32_t>(mc::MoveMode::Jump): return mc::MoveMode::Jump;
        default:                                             return mc::MoveMode::Run;
    }
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
                                                 std::uint64_t now_ms) const {
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

    // The Δt this packet is measured over: time since the last processed intent
    // (or since spawn for the first one), floored to one tick.
    const double dt = elapsed_seconds(last_time_ms_, now_ms);
    const float step = horizontal_distance(authoritative_, intent.pos);

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
    // Prune first so the window reflects "as of now" before we add this sample.
    prune_window(now_ms);

    if (decision.accepted) {
        const float step = horizontal_distance(authoritative_, decision.pos);
        authoritative_ = decision.pos;
        window_.push_back(WindowSample{now_ms, step});
    }
    // Rejected: authoritative_ UNCHANGED (snap-back). No window sample (a rejected
    // move must not contribute to the sustained-speed budget).

    // Either way, the intent WAS processed: record it so the next Δt / rate check
    // measures from here.
    processed_any_ = true;
    last_seq_ = intent.seq;
    last_flags_ = intent.state_flags;
    last_time_ms_ = now_ms;
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
