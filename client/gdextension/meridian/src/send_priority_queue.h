// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — the outbound priority send queue for the net thread (issue
// #97, scope item 3). One lock-free SPSC ring (spsc_ring.h) PER priority; the net
// thread drains them in STRICT priority order so a control frame never queues behind
// a bulk transfer and a chatty bulk stream can never starve real-time movement.
//
// The producer is the MAIN thread (push at a priority); the consumer is the NET
// thread (drain). Each ring is a genuine single-producer/single-consumer channel, so
// the SPSC ring stays the ONLY cross-thread channel (net_thread_core.h) — this class
// is just an array of them plus the priority drain policy.
//
// ENGINE-FREE (Client SAD §9.2): header-only, plain C++17. The drain policy is
// unit-tested deterministically (single-threaded, a recording sink) in
// net_thread_core_test.cpp.

#ifndef MERIDIAN_SEND_PRIORITY_QUEUE_H
#define MERIDIAN_SEND_PRIORITY_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "net_thread_messages.h"  // Bytes, SendPriority, kSendPriorityCount, priority_index
#include "spsc_ring.h"

namespace meridian::net {

// Per-priority outbound ring capacity: the main thread produces at most a few frames
// per tick per class, so this absorbs many ticks of back-pressure headroom.
inline constexpr std::size_t kSendRingCapacity = 256;

class SendPriorityQueue {
public:
    SendPriorityQueue() {
        for (auto& ring : rings_) {
            ring = std::make_unique<SpscRing<Bytes, kSendRingCapacity>>();
        }
    }

    SendPriorityQueue(const SendPriorityQueue&) = delete;
    SendPriorityQueue& operator=(const SendPriorityQueue&) = delete;

    // PRODUCER (main thread). Enqueue `frame` at `prio`. Returns false if that
    // priority's ring is full (the caller decides drop / back-pressure). Never blocks.
    bool push(Bytes frame, SendPriority prio) {
        const std::size_t idx = priority_index(prio);
        if (idx >= kSendPriorityCount) return false;
        return rings_[idx]->push(std::move(frame));
    }

    // CONSUMER (net thread). Drain frames to `sink` in STRICT priority order:
    // kControl fully, then kMovement fully, then up to `bulk_budget` kBulk frames
    // (so bulk yields to control/movement). `sink(const Bytes&)` returns false to
    // ABORT the drain (e.g. a socket write failed). Returns false iff the sink
    // aborted; `*out_sent`, when non-null, receives how many frames were sent.
    template <typename Sink>
    bool drain(Sink&& sink, int bulk_budget, std::uint64_t* out_sent = nullptr) {
        std::uint64_t sent = 0;
        bool ok = true;
        for (std::size_t idx = 0; idx < kSendPriorityCount && ok; ++idx) {
            const bool is_bulk = (idx == priority_index(SendPriority::kBulk));
            int budget = is_bulk ? bulk_budget : -1;  // -1 = unbounded
            Bytes frame;
            while ((budget != 0) && rings_[idx]->pop(frame)) {
                if (!sink(frame)) {
                    ok = false;
                    break;
                }
                ++sent;
                if (budget > 0) --budget;
            }
        }
        if (out_sent) *out_sent = sent;
        return ok;
    }

    // Approximate emptiness across all priorities (racy — stats/tests only).
    bool empty_approx() const {
        for (const auto& ring : rings_) {
            if (!ring->empty_approx()) return false;
        }
        return true;
    }

private:
    std::unique_ptr<SpscRing<Bytes, kSendRingCapacity>> rings_[kSendPriorityCount];
};

}  // namespace meridian::net

#endif  // MERIDIAN_SEND_PRIORITY_QUEUE_H
