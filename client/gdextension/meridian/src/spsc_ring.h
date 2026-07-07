// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — lock-free single-producer / single-consumer ring buffer
// (issue #97). The ONE cross-thread channel between the dedicated net thread and
// the Godot main thread: the net thread PRODUCES decoded inbound messages into an
// inbound ring the main thread CONSUMES at the pre-sim sync point, and the main
// thread PRODUCES outbound frames into per-priority rings the net thread CONSUMES.
//
// ENGINE-FREE (Client SAD §9.2): no Godot types, plain C++17 + <atomic>. So this
// primitive is unit-tested WITHOUT a Godot runtime (concurrent producer/consumer,
// wrap-around, no lost/duplicated elements) in a plain ctest.
//
// MEMORY ORDERING (the correctness argument — the ONLY shared state is head_/tail_
// and the slot storage they guard):
//
//   * Exactly ONE producer thread ever calls push(); exactly ONE consumer thread
//     ever calls pop(). This is a hard precondition — two producers or two
//     consumers is undefined. (Each direction of the net binding upholds it: the
//     inbound ring's producer is the net thread and consumer is the main thread;
//     each outbound ring's producer is the main thread and consumer is the net
//     thread.)
//
//   * head_ is the write index, owned by the producer. tail_ is the read index,
//     owned by the consumer. Each is written by exactly one thread, so neither is
//     a read-modify-write race; they are plain atomics used purely to publish a
//     value across the thread boundary with the right fences.
//
//   * PRODUCER (push): writes the element into slot[head], THEN head_.store(next,
//     release). The release store orders the slot write BEFORE the index
//     publication. The consumer's tail_.load(acquire) at the top of push()
//     synchronises-with the consumer's release store of tail_ in pop(), so the
//     producer sees freed slots.
//
//   * CONSUMER (pop): reads head_.load(acquire) — synchronises-with the producer's
//     release store — guaranteeing the slot write HAPPENS-BEFORE this load, so the
//     element read from slot[tail] is fully constructed. After moving the element
//     out it does tail_.store(next, release) to publish the freed slot to the
//     producer.
//
//   * Capacity is fixed at compile time; the backing array carries one always-empty
//     sentinel slot to disambiguate full (head+1 == tail) from empty (head == tail)
//     without a separate count — the classic lock-free SPSC trick, so neither index
//     is ever a read-modify-write.
//
// CLEAN-ROOM: the algorithm is the textbook Lamport SPSC circular buffer with
// acquire/release fences (C++ memory model). No GPL source consulted.

#ifndef MERIDIAN_SPSC_RING_H
#define MERIDIAN_SPSC_RING_H

#include <atomic>
#include <cstddef>
#include <utility>

namespace meridian::net {

// A bounded lock-free SPSC ring holding up to `Capacity` live elements of type T.
// The backing array has Capacity+1 slots (one always empty). T must be movable.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 1, "SpscRing needs capacity >= 1");

public:
    SpscRing() = default;
    ~SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // The number of live elements the ring can hold (excludes the sentinel slot).
    static constexpr std::size_t capacity() { return Capacity; }

    // PRODUCER ONLY. Move `value` into the ring. Returns false (and leaves `value`
    // untouched) if the ring is full — the caller decides the drop/back-pressure
    // policy. Never blocks.
    bool push(T&& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);
        // acquire: see the consumer's latest tail so a slot it freed is visible.
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        slots_[head] = std::move(value);
        // release: publish the slot write before advancing the index.
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Copy overload for convenience (copies into a temporary, then moves).
    bool push(const T& value) {
        T tmp = value;
        return push(std::move(tmp));
    }

    // CONSUMER ONLY. Move the front element into `out`. Returns false (leaving
    // `out` untouched) if the ring is empty. Never blocks.
    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // acquire: synchronise-with the producer's release store so the slot the
        // producer wrote HAPPENS-BEFORE we read it below.
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = std::move(slots_[tail]);
        // release: publish the freed slot to the producer.
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

    // Approximate size (racy across threads — for stats/tests, not control flow).
    std::size_t size_approx() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head + kSlots - tail) % kSlots;
    }

    // Approximate emptiness (racy — a consumer's own view is exact for itself).
    bool empty_approx() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    // One extra sentinel slot disambiguates full from empty.
    static constexpr std::size_t kSlots = Capacity + 1;

    static constexpr std::size_t increment(std::size_t i) { return (i + 1) % kSlots; }

    T slots_[kSlots]{};
    std::atomic<std::size_t> head_{0};  // producer's write index
    std::atomic<std::size_t> tail_{0};  // consumer's read index
};

}  // namespace meridian::net

#endif  // MERIDIAN_SPSC_RING_H
