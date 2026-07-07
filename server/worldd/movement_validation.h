// SPDX-License-Identifier: Apache-2.0
//
// worldd — movement intake + validation v0 (issue #86; server movement authority).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §5.5 (the OPS-03 movement
// validation envelope: rate class, per-packet + sliding-2 s-window speed check,
// map bounds, z-vs-heightfield tolerance, snap-back correction policy), §3.2 (the
// 50 ms / 20 Hz map tick that advances authoritative state), §2.5/§6 (game state
// is single-threaded on the world thread), decision D-19 (M0 flat bootstrap map,
// bounds-only), and docs/movement-spike.md / movement_constants.h (#101, the
// LOCKED shared constants). No GPL source consulted. See CONTRIBUTING.md.
//
// WHY THIS IS "SERVER IS LAW" (Baseline Pillar 3): MovementIntent is the ONE
// client input worldd trusts, and only AFTER validation (SAD §5.5, world.fbs
// MovementIntent doc). The server never trusts the client's claimed position; it
// re-derives the authoritative position itself. A valid intent advances the
// session's authoritative position; an invalid one is REJECTED and answered with
// a CORRECTION MovementState that snaps the client back to the last authoritative
// position — the client reconciles to the server, not the other way round.
//
// PURITY (so it is DB-free unit-testable): the core is a PURE function
//   validate_move(authoritative_state, intent, dt, now) -> Decision
// with NO I/O, NO DB, NO clock, NO socket. The session/integration path (which
// needs a WorldSession) just feeds it decoded intents and applies the Decision.
// This is what lets the movement unit test run in the plain server ctest with no
// MariaDB (unlike #84's grant test).
//
// ─── VALIDATION RULES (v0 = "speed + bounds", SAD §5.5, M0 exit row) ─────────
// For each accepted-into-intake MovementIntent, the validator checks, in order:
//
//   R1. RATE CLASS (SAD §5.5 "≤ 10/s/client + state changes"; #101
//       kMovementIntentMaxHz). Intents arriving faster than 10/s that are NOT a
//       state change (state_flags unchanged) are DROPPED/coalesced at intake —
//       they never reach the speed/bounds checks. A state change (state_flags
//       differs from the last processed intent) is always admitted (mode toggles
//       must not be rate-limited away). Enforced by MovementIntake, below.
//
//   R2. PER-PACKET SPEED (SAD §5.5 "displacement vs server_speed(active_mode) ×
//       Δt × 1.15 per packet"). Horizontal displacement (x,y) from the last
//       authoritative position must be ≤ server_speed(mode) × dt × kSpeedTolerance.
//       Δt is the time since the last processed intent for this session (clamped
//       to a sane floor so a burst of same-instant packets cannot divide-by-zero
//       into an infinite budget).
//
//   R3. SLIDING-WINDOW SPEED (SAD §5.5 "AND over a sliding 2 s window (catches
//       burst-then-idle cheats)"). The sum of accepted horizontal displacements
//       over the trailing kSpeedWindowSeconds must be ≤ server_speed(mode) ×
//       window_elapsed × kSpeedTolerance. This catches a cheat that stays under
//       the per-packet cap but sustains it far past what a legit mover could.
//
//   R4. MAP BOUNDS (SAD §5.5 "inside map bounds"). The proposed (x,y) must lie
//       inside the M0 bootstrap play area [kZoneMinXY, kZoneMaxXY]² (D-19; the
//       128 m chunk footprint stands in for real zone bounds until Zone-01/M1).
//
//   R5. Z-vs-GROUND (SAD §5.5 "z within ±4 m of heightfield/navmesh sample";
//       #101 kHeightTolerance). |proposed_z − ground(x,y)| ≤ 4 m. At M0 (D-19)
//       ground(x,y) is the flat plane kFlatGroundZ = 0 (bounds-only, no
//       heightfield consumption yet).
//
// Any R2..R5 violation ⇒ REJECT ⇒ snap-back correction MovementState at the last
// authoritative position (the mover reconciles). R1 rate excess ⇒ silently
// dropped (no correction — it is throttling, not a cheat). The v0 policy ladder
// stops at "correct (snap-back)"; the SAD §5.5 "N violations/min ⇒ kick ⇒ audit"
// escalation is a later story (v1 full envelope, M0-exit row shows v0 = speed +
// bounds only).

#ifndef MERIDIAN_WORLDD_MOVEMENT_VALIDATION_H
#define MERIDIAN_WORLDD_MOVEMENT_VALIDATION_H

#include <cstdint>
#include <deque>

#include "movement_constants.h"

namespace meridian::worldd {

// A minimal 3D position + facing in zone-local metres/radians. Mirrors the
// MovementIntent/MovementState position fields (schema/net/world.fbs) without
// pulling in the FlatBuffers types — keeps the validator pure and independently
// testable. z is the vertical (height) axis; the horizontal plane is (x, y),
// matching the world.fbs field naming (x, y, z, orientation).
struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;  // radians (not validated at v0 — any facing legal)
};

// A decoded, plain-old-data view of a MovementIntent (schema/net/world.fbs
// MovementIntent), lifted off the FlatBuffer so the validator has no wire
// dependency. The dispatcher decodes the FlatBuffer once and fills this.
struct MovementIntentPod {
    std::uint32_t seq = 0;             // per-client monotonic (ack / reconciliation)
    std::uint32_t state_flags = 0;     // movement-mode bitfield (walk/run/jump/…)
    Position pos;                      // proposed position + facing
    std::uint64_t client_time_ms = 0;  // client clock at capture (ClockSync-keyed)
};

// Why an intent was rejected (server-side detail; the wire correction is the same
// snap-back MovementState regardless, denying the client an oracle for WHICH
// check tripped — same principle as the grant-reject reasons in #84).
enum class MoveReject : std::uint8_t {
    kNone = 0,          // accepted (not a rejection)
    kSpeedPerPacket,    // R2 — per-packet displacement over cap
    kSpeedWindow,       // R3 — sliding-2 s-window displacement over cap
    kOutOfBounds,       // R4 — outside map bounds
    kZOutOfRange,       // R5 — z too far from ground sample
};

// The outcome of validating one intake-admitted intent. `accepted` says whether
// the move advances the authoritative position; `reject` names the failing check
// (kNone when accepted). `state` is the authoritative MovementState to broadcast
// EITHER WAY (SAD §5.5): on accept it is the advanced position; on reject it is
// the snap-back correction at the last authoritative position. ack_seq always
// echoes the intent's seq so the mover can reconcile (world.fbs MovementState).
struct MoveDecision {
    bool accepted = false;
    MoveReject reject = MoveReject::kNone;

    // The authoritative MovementState fields (schema/net/world.fbs MovementState).
    // entity_guid + server_time_ms are filled by the caller (session-scoped);
    // this pure result carries the position/flags/ack the validator determines.
    std::uint32_t ack_seq = 0;
    std::uint32_t state_flags = 0;
    Position pos;  // advanced (accept) or last-authoritative (snap-back)
};

// Decode the active MoveMode from a MovementIntent.state_flags bitfield. M0 modes
// are walk/run/jump (movement_constants.h §2). At v0 the flags carry the mode
// selector in the low bits; the mapping is intentionally simple and shared in
// spirit with the client's decode (the exact bit layout is a #102/#101 follow-up —
// v0 needs only the speed-cap-selecting mode). Unknown/zero ⇒ Run (the default
// forward locomotion cap), which is the SAFE default: it grants the LARGEST M0
// ground-speed budget, so an unknown flag never spuriously rejects a legal move —
// the speed cap only tightens for Walk/Idle. (Swim is M1-reserved; a Swim bit is
// treated as Run at M0.)
movement::MoveMode mode_from_flags(std::uint32_t state_flags);

// ---------------------------------------------------------------------------
// The pure validator + per-session authoritative state.
// ---------------------------------------------------------------------------

// One session's authoritative movement state, owned by the WORLD THREAD (SAD
// §2.5/§6: game state is single-threaded — IO workers hand intents over the queue;
// this struct is never touched by an IO worker). It holds the last authoritative
// position the server derived, the last processed intent's seq/flags/time (for
// rate-class + Δt), and the trailing sliding-window displacement samples.
//
// It is a plain struct with a small helper API so a unit test can construct it,
// drive validate_move()/apply on it, and assert the authoritative position with
// no session, socket, or DB.
class SessionMovementState {
public:
    // Start authoritative state at a spawn position (enter-world hands the D-11
    // placeholder's spawn point; at M0 that is the flat-world origin unless
    // overridden). `spawn_time_ms` seeds the "last processed" clock so the first
    // intent's Δt is measured from spawn.
    explicit SessionMovementState(Position spawn = {}, std::uint64_t spawn_time_ms = 0);

    // The current authoritative position (what a snap-back corrects to, and what
    // an accepted move advances). Read-only accessor for tests + the AoI relay.
    const Position& authoritative() const { return authoritative_; }

    // The mover's entity guid (filled from the session's placeholder character;
    // 0 until set). Carried here so the world thread can stamp MovementState.
    std::uint64_t entity_guid() const { return entity_guid_; }
    void set_entity_guid(std::uint64_t guid) { entity_guid_ = guid; }

    // Have we processed at least one intent yet? (Δt / window seeding depends on
    // it; the first intent measures Δt from the spawn time.)
    bool has_processed() const { return processed_any_; }

    // Validate one intake-admitted intent against R2..R5 (PURE — no clock, no I/O:
    // `now_ms` is passed in, and equals intent.client_time_ms at the session path,
    // or a test-supplied value in unit tests). Returns the MoveDecision WITHOUT
    // mutating state — the caller applies it via apply(). Splitting validate from
    // apply lets a test check a decision without committing it, and lets the
    // caller stamp session fields onto the state before broadcast.
    MoveDecision validate_move(const MovementIntentPod& intent, std::uint64_t now_ms) const;

    // Commit a decision: on an ACCEPTED move, advance the authoritative position,
    // push the displacement into the sliding window, and record seq/flags/time as
    // "last processed". On a REJECTED move, the authoritative position is
    // UNCHANGED (snap-back), but seq/flags/time are still recorded as processed
    // (the intent WAS handled — the next Δt/rate check measures from here) and NO
    // displacement is added to the window (a rejected move never contributes to
    // the sustained-speed budget). Idempotent per decision; call once per
    // validate_move result.
    void apply(const MoveDecision& decision, const MovementIntentPod& intent,
               std::uint64_t now_ms);

    // Sliding-window bookkeeping accessor (test/diagnostic): total displacement
    // currently retained in the trailing kSpeedWindowSeconds window.
    float window_displacement() const;

    // The seq/flags/time of the last processed intent (test/diagnostic + the
    // MovementIntake rate class reads flags to detect a state change).
    std::uint32_t last_seq() const { return last_seq_; }
    std::uint32_t last_flags() const { return last_flags_; }
    std::uint64_t last_time_ms() const { return last_time_ms_; }

private:
    // A single window sample: the client time it was accepted at + the horizontal
    // displacement it added. Pruned once older than kSpeedWindowSeconds.
    struct WindowSample {
        std::uint64_t time_ms;
        float displacement;
    };

    void prune_window(std::uint64_t now_ms);

    Position authoritative_;
    std::uint64_t entity_guid_ = 0;

    bool processed_any_ = false;
    std::uint32_t last_seq_ = 0;
    std::uint32_t last_flags_ = 0;
    std::uint64_t last_time_ms_ = 0;

    std::deque<WindowSample> window_;  // trailing 2 s displacement samples
};

// ---------------------------------------------------------------------------
// Intake rate limiter (R1) — separate from validation so it can drop an intent
// BEFORE the speed/bounds checks (a throttled intent is not a cheat; it never
// produces a correction).
// ---------------------------------------------------------------------------

// Per-session movement-intent intake gate (SAD §5.5 rate class, #101
// kMovementIntentMaxHz). admit() returns true if the intent should proceed to
// validation, false if it is dropped/coalesced by the ≤ 10/s cap. A STATE CHANGE
// (state_flags differs from the last admitted intent) is ALWAYS admitted (mode
// toggles must not be throttled — SAD §5.5 "+ state changes"). Non-state-change
// intents are admitted only if ≥ (1000 / 10) ms have elapsed since the last
// admitted one. Owned by the world thread alongside SessionMovementState.
class MovementIntake {
public:
    // Decide whether `intent` (arriving at `now_ms`) is admitted for validation.
    // Updates the last-admitted timestamp/flags on an admit. `now_ms` is the
    // client_time_ms at the session path (a monotonic-ish client clock keyed to
    // ClockSync) or a test value.
    bool admit(const MovementIntentPod& intent, std::uint64_t now_ms);

    // How many intents this gate has DROPPED (test/diagnostic + the
    // meridian_opcode_dropped_total metric hook later).
    std::uint64_t dropped() const { return dropped_; }

    // How many it has ADMITTED (test/diagnostic).
    std::uint64_t admitted() const { return admitted_; }

private:
    bool have_admitted_ = false;
    std::uint32_t last_flags_ = 0;
    std::uint64_t last_admit_ms_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t admitted_ = 0;
};

// The minimum inter-intent spacing (ms) the rate class allows for a
// non-state-change intent: 1000 / kMovementIntentMaxHz = 100 ms at 10/s.
inline constexpr std::uint64_t kMinIntentSpacingMs =
    static_cast<std::uint64_t>(1000 / movement::kMovementIntentMaxHz);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_MOVEMENT_VALIDATION_H
