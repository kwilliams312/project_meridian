// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-bus — non-template internals for the typed message identity.
// CLEAN-ROOM from the server SAD (§2.6). See message.hpp.

#include "meridian/bus/message.hpp"

#include <atomic>

namespace meridian::bus {

namespace detail {

MessageTypeId next_message_type_id() noexcept {
    // A single process-wide monotonic counter. message_type_id<T>() calls this
    // exactly once per distinct T (from a function-local static), so each type
    // gets a unique, stable id for the life of the process. Relaxed is fine: the
    // per-T static's own initialisation is the ordering guarantee (the C++
    // memory model serialises static-init), and the value only needs to be
    // unique, not ordered across types.
    static std::atomic<MessageTypeId> counter{1};  // 0 reserved as "unassigned"
    return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace detail

const char* lane_name(Lane lane) noexcept {
    switch (lane) {
        case Lane::kControl: return "control";
        case Lane::kSession: return "session";
        case Lane::kBulk:    return "bulk";
    }
    return "unknown";
}

}  // namespace meridian::bus
