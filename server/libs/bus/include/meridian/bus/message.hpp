// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-bus — typed message identity + the bus envelope.
//
// CLEAN-ROOM: designed from the server SAD only — §2.6 (the message bus: "the
// single mechanism for any effect that crosses a map boundary or targets a
// global manager", `bus.send(Destination, TypedMessage)`, typed messages,
// MPSC inboxes drained at tick boundaries), §5.6 (the BusEnvelope shape:
// ver / src / dst Address / corr_id / lane / payload_type / payload — v0 keeps
// the in-process subset), §6.1/§6.3 (lanes: control / session / bulk; credit
// backpressure; "no map-thread code may block on bus.send"). No GPL source
// (CMaNGOS / TrinityCore or otherwise) was consulted. See CONTRIBUTING.md.
//
// This header defines the TYPE identity the bus routes on. The SAD's wire
// envelope is FlatBuffer-typed with a u16 `payload_type` (§5.6); v0 is
// in-process, so instead of a wire tag we use the C++ type itself as the
// routing key via a stable per-type id. The seam is deliberate: when the M2
// mesh transport lands (§7), `MessageTypeId` becomes the envelope's
// `payload_type` and the payload is FlatBuffer bytes — the pub/sub API above
// it does not change. That is exactly the "transport swap, not rewrite"
// property the SAD's M0 bus rule buys (§7 "M0/M1 unchanged").

#pragma once

#include <cstdint>

namespace meridian::bus {

// A stable, process-unique identity for a message type. In v0 this is derived
// from the C++ type (message_type_id<T>()); at the M2 mesh split it maps onto
// the BusEnvelope's u16 `payload_type` (SAD §5.6). Routing compares these, so
// a subscriber for T only ever receives T — the "typed messages" invariant.
using MessageTypeId = std::uint32_t;

namespace detail {
// Monotonic id source. Each distinct T that reaches message_type_id<T>() takes
// the next id on first use. Process-local and order-of-first-use dependent by
// design (v0 is in-process); it is never serialized, so cross-process identity
// is the M2 concern (a declared u16 in world.fbs), not this counter's.
MessageTypeId next_message_type_id() noexcept;
}  // namespace detail

// The routing key for message type T. Two calls with the same T return the same
// id for the life of the process; two different types never collide. `const T`,
// `T&`, and `T` share one id (std::decay-like via the template being keyed on
// the bare T the caller names — callers always name the bare type).
template <typename T>
MessageTypeId message_type_id() noexcept {
    // Function-local static: initialised once, on first use, per distinct T.
    static const MessageTypeId id = detail::next_message_type_id();
    return id;
}

// The bus lanes (SAD §5.6 "Links carry multiplexed lanes", §6.3). A lane is an
// ordered, independently-backpressured delivery class. The SAD's three:
//   - kControl : lifecycle / placement / transfer commands. Reserved credits —
//                a saturated data lane must never stall control (§6.3). In v0
//                the guarantee is expressed as: control lanes are configured
//                with their own bounded depth, independent of data lanes.
//   - kSession : per-session gameplay traffic (gateway <-> worker, §6.1).
//   - kBulk    : fan-out / DB-adjacent bulk (chat fan-out, save batches).
// The lane is part of the subscription key: a subscriber names {type, lane}.
enum class Lane : std::uint8_t {
    kControl = 0,
    kSession = 1,
    kBulk = 2,
};

// Human-readable lane name for logs / metrics / test diagnostics.
const char* lane_name(Lane lane) noexcept;

}  // namespace meridian::bus
