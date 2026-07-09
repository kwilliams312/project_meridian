// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-opcode RATE CLASSES for the dispatcher (OPS-03b, #421; epic #21).
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md §4-M1 (OPS-03 "per-opcode rate
// classes") + §6 ("separate append-only audit streams for … anti-cheat flags"),
// docs/sad/server-sad.md §5.2 (the IF-2 opcode registry) / §5.5 (the anti-cheat
// throughput envelope), and the OPS-05 audit facility (meridian/core/audit.hpp,
// #92). No GPL source consulted. See CONTRIBUTING.md.
//
// WHAT THIS IS. A SERVER-SIDE table keyed by opcode that assigns every client
// opcode a RATE CLASS (session / move / chat / action), plus a per-connection,
// per-class sliding-window gate the dispatcher runs at the dispatch entry (per
// ConnCtx). A frame that exceeds its class ceiling is DROPPED (not disconnected —
// throttling is not a protocol error) and flagged on the append-only anti-cheat
// audit stream (action="rate_limited"). Deliberately NOT in the wire schema: the
// class ↔ opcode mapping is a pure server policy (no world.fbs / net-conformance
// change), so it can be re-tuned without a protocol revision.
//
// INTERACTION WITH THE EXISTING PER-FEATURE CAPS (do NOT double-reject). Movement
// and chat already carry FINER per-feature gates inside their handlers:
//   * MOVEMENT_INTENT — MovementIntake (≤ kMovementIntentMaxHz = 10/s admitted,
//     coalescing the rest silently; #86/#420), and
//   * CHAT_MESSAGE    — ChatIntake (≤ kChatMaxPerSecond = 5/s, typed reject; #367).
//
// The MOVE class DEFERS ENTIRELY to MovementIntake — the dispatcher does NOT run
// this gate for MOVEMENT_INTENT (see the dispatch entry). MovementIntake is the
// authoritative movement rate limiter, and crucially it is keyed on the intent's
// CLIENT time + state-change semantics, not the server's wall clock. A legitimate
// client paces intents by its OWN clock (which need not track the server's wall
// time — a client may stream a burst of already-timestamped intents), so a second,
// wall-clock ceiling here would double-reject legitimately-paced movement. Movement
// flooding is therefore handled by MovementIntake alone; the move class exists only
// for classification/metric symmetry.
//
// The CHAT class is ENFORCED here as a connection-wide backstop ABOVE ChatIntake:
// ChatIntake's 5/s typed cap remains the primary chat gate, and the chat ceiling
// here sits well above it, so a compliant sender never trips the backstop. The
// SESSION and ACTION classes have NO finer per-feature gate — the dispatcher rate
// class IS their only throughput bound. The ceilings are set well above any
// legitimate interactive rate, so the classes only ever catch a gross flood.

#ifndef MERIDIAN_WORLDD_RATE_CLASS_H
#define MERIDIAN_WORLDD_RATE_CLASS_H

#include <array>
#include <cstdint>
#include <deque>

#include "meridian/core/audit.hpp"
#include "world_generated.h"  // net::Opcode (world.fbs opcode registry)

namespace meridian::worldd {

// The rate class an opcode belongs to. Each names a throughput budget a legitimate
// client stays well within; the classes are coarse on purpose (a small, closed set
// keeps the metric/audit `reason` label low-cardinality).
enum class RateClass : std::uint8_t {
    kSession = 0,  // session / system + character management (handshake, clock,
                   //   char list/create/delete, enter-world) — low, bursty at login.
    kMove,         // MOVEMENT_INTENT — DEFERRED to MovementIntake (not enforced at
                   //   the dispatcher; see the file header). Classification only.
    kChat,         // CHAT_MESSAGE (incl. GM commands riding it) — a flood ceiling
                   //   ABOVE ChatIntake's 5/s typed cap.
    kAction,       // gameplay actions: cast, quest, loot, vendor, gossip, trainer,
                   //   death/resurrect — the interactive request set.
};

// The number of rate classes (array sizing for the per-connection gate).
inline constexpr std::size_t kRateClassCount = 4;

// The per-session limit for a class: the maximum frames ADMITTED per rolling
// kRateWindowMs window on ONE connection. Exceeding it drops the frame.
//
// These are FLOOD / DoS ceilings, NOT fine gameplay-rate limiters — the dispatcher
// rate class is the connection-wide anti-cheat THROUGHPUT backstop (server PRD §6
// "anti-cheat throughput"); the fine per-second gameplay rates live in the feature
// gates (MovementIntake 10/s, ChatIntake 5/s). So the ceilings sit FAR above any
// legitimate burst: a human client sends a handful of session/action opcodes per
// second, and even a back-to-back scripted client (e.g. the IT-M1 10-quest chain
// integration harness, which fires the whole chain's ~40 action opcodes in one
// sub-100 ms burst) stays well under. Only a gross flood (hundreds/s) — a DoS or a
// dupe-spam attempt — trips a class. Re-tune freely: this is pure server policy,
// no wire/schema change (see the file header).
inline constexpr int kSessionRatePerWindow = 100;  // handshake/char-mgmt/clock/enter
inline constexpr int kMoveRatePerWindow    = 60;   // (deferred to MovementIntake;
                                                   //  unused by the dispatcher gate)
inline constexpr int kChatRatePerWindow    = 100;  // backstop far above ChatIntake's 5/s
inline constexpr int kActionRatePerWindow  = 256;  // cast/quest/loot/vendor/gossip/etc.

// The sliding rate window (ms). One second, matching ChatIntake's window (#367).
inline constexpr std::uint64_t kRateWindowMs = 1000;

// The SERVER-SIDE rate-class table: the class an opcode is enforced under. A pure
// function of the opcode (no wire dependency). Unlisted opcodes fall back to
// kSession (the most generous ceiling) so a newly-handled opcode is never silently
// throttled before it is classified here.
RateClass rate_class_for(net::Opcode op);

// The per-window admit ceiling for a class (kSessionRatePerWindow, …).
int rate_class_limit(RateClass rc);

// The stable, low-cardinality class name — the audit `reason` + metric label value
// ("session" / "move" / "chat" / "action").
const char* rate_class_name(RateClass rc);

// ---------------------------------------------------------------------------
// RateLimiter — the per-connection, per-class sliding-window gate.
// ---------------------------------------------------------------------------
// One per connection, owned on ConnCtx and single-threaded on that connection's IO
// worker (like MovementIntake / ChatIntake), so it needs no locking. Holds one
// sliding-kRateWindowMs bucket per class; admit() prunes expired stamps, admits if
// the bucket is under the class ceiling, else drops. `now_ms` is a monotonic ms
// clock supplied by the caller (steady_clock at the dispatch path; a fixed value in
// unit tests) — the gate takes NO clock itself, so it is deterministically testable.
class RateLimiter {
public:
    // Admit a frame of class `rc` arriving at `now_ms`. Returns true if it is within
    // the class ceiling for the trailing window (and records the admit); false if it
    // exceeds the ceiling (a drop). Prunes stamps older than kRateWindowMs first.
    bool admit(RateClass rc, std::uint64_t now_ms);

    // How many frames this gate has DROPPED / ADMITTED for a class (test/diagnostic).
    std::uint64_t dropped(RateClass rc) const;
    std::uint64_t admitted(RateClass rc) const;

private:
    struct Bucket {
        std::deque<std::uint64_t> window;  // admit timestamps within the last window
        std::uint64_t dropped = 0;
        std::uint64_t admitted = 0;
    };
    std::array<Bucket, kRateClassCount> buckets_{};
};

// Build the append-only ANTI-CHEAT audit record for one rate-class flood drop
// (server PRD §6). `account_id`/`grant_id` attribute + correlate the flag to the
// session (0 => omitted); `op` is the throttled opcode; `rc` its class (the audit
// `reason`). PURE (no socket/clock/DB) so a unit test asserts the exact record the
// dispatch emits. The dispatch emits the result via core::audit::emit.
core::audit::Record build_rate_limited_audit(std::uint64_t account_id,
                                             std::uint64_t grant_id, net::Opcode op,
                                             RateClass rc);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_RATE_CLASS_H
