// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — the cross-thread message types + outbound priority scheme for
// the GDExtension net thread (issue #97). Engine-free (Client SAD §9.2): plain
// C++17, no Godot types, so the net-thread core + these types are unit-tested
// without a Godot runtime.
//
// Two kinds of cross-thread traffic flow over the SPSC rings (spsc_ring.h):
//
//   INBOUND  (net thread -> main thread): decoded server events. The net thread
//            decodes the IF-2 frame header (wire_frame.h) and — for the messages
//            the client acts on immediately — the FlatBuffer body (codec.h) OFF the
//            main thread, then enqueues an InboundMessage. Entity-relay frames
//            (EntityEnter/Update/Leave, not in the net-core codec) are forwarded as
//            raw opcode+seq+payload for the main-thread `sim` layer to decode.
//
//   OUTBOUND (main thread -> net thread): fully-encoded IF-2 frame bytes ready for
//            ITransport::send_frame, tagged with a SendPriority (below).

#ifndef MERIDIAN_NET_THREAD_MESSAGES_H
#define MERIDIAN_NET_THREAD_MESSAGES_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "meridian/clientnet/codec.h"   // codec::MovementState / Disconnect
#include "meridian/clientnet/framing.h"  // clientnet::Bytes

namespace meridian::net {

using Bytes = meridian::clientnet::Bytes;

// ---------------------------------------------------------------------------
// Outbound send priorities (documented scheme, issue #97 scope item 3).
// ---------------------------------------------------------------------------
//
// The net thread owns ONE SPSC ring per priority and drains them in strict order
// each loop iteration: kControl first (fully), then kMovement (fully), then up to a
// bounded budget of kBulk. So a control frame never queues behind a bulk transfer,
// and a chatty bulk stream can never starve real-time movement. Priorities are a
// small fixed set — deliberately coarse; finer QoS is a later concern.
enum class SendPriority : std::uint8_t {
    // Latency-critical, tiny, must never be delayed: WorldHello, heartbeat/keepalive,
    // ClockSync, and the client-initiated Disconnect. Drained first, in full.
    kControl = 0,
    // Real-time gameplay: MovementIntent. Drained after control, in full.
    kMovement = 1,
    // Best-effort, potentially large or bursty (chat, inventory sync, asset pulls).
    // Drained last, capped per iteration so it yields to control/movement.
    kBulk = 2,
};

// The number of distinct priorities — the count of outbound rings.
inline constexpr std::size_t kSendPriorityCount = 3;

// Ordinal of a priority (its ring index). Lower = higher priority = drained first.
inline constexpr std::size_t priority_index(SendPriority p) {
    return static_cast<std::size_t>(p);
}

// ---------------------------------------------------------------------------
// Inbound messages (net thread -> main thread).
// ---------------------------------------------------------------------------

// What an InboundMessage carries. The net thread classifies each decoded IF-2 frame
// into exactly one kind so the main thread can branch without re-decoding.
enum class InboundKind : std::uint8_t {
    kHandshakeOk = 0,     // entered the world (reply to WorldHello)
    kMovementState = 1,   // authoritative MovementState (state valid)
    kDisconnect = 2,      // server Disconnect (disc valid)
    kEntityFrame = 3,     // EntityEnter/Update/Leave — raw (opcode/seq/payload)
    kTransportClosed = 4, // peer closed / socket error (session over)
    kConnectFailed = 5,   // the net thread could not establish the world connection
    // Character management + server-authoritative enter-world (D-35 / #286 / #341),
    // decoded off the main thread; the matching field below is populated.
    kCharList = 6,        // CharListResponse (roster valid)
    kCharCreate = 7,      // CharCreateResponse (char_create valid)
    kCharDelete = 8,      // CharDeleteResponse (char_delete valid)
    kEnterWorld = 9,      // EnterWorldResponse (enter_world valid) — OK means spawned
};

// One decoded server event handed to the main thread via the inbound SPSC ring.
// Movable (owns a vector + string); moves are cheap. Only the field named by `kind`
// is meaningful — the rest are default.
struct InboundMessage {
    InboundKind kind = InboundKind::kTransportClosed;

    // Raw IF-2 frame header (always populated for a decoded frame; 0 otherwise).
    std::uint16_t opcode = 0;
    std::uint64_t seq = 0;

    // Valid when kind == kMovementState.
    meridian::clientnet::codec::MovementState state;

    // Valid when kind == kDisconnect.
    meridian::clientnet::codec::Disconnect disc;

    // Valid when kind == kCharList / kCharCreate / kCharDelete / kEnterWorld
    // (character-select round-trips, D-35). Only the one named by `kind` is set.
    meridian::clientnet::codec::CharListResponse roster;
    meridian::clientnet::codec::CharCreateResponse char_create;
    meridian::clientnet::codec::CharDeleteResponse char_delete;
    meridian::clientnet::codec::EnterWorldResponse enter_world;

    // Valid when kind == kEntityFrame: the raw FlatBuffer body (the main-thread sim
    // layer decodes EntityEnter/Update/Leave from it). Also carries a human note on
    // kConnectFailed / kTransportClosed.
    Bytes payload;
    std::string detail;
};

}  // namespace meridian::net

#endif  // MERIDIAN_NET_THREAD_MESSAGES_H
