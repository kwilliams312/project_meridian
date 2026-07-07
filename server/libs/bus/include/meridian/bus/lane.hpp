// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-bus — a bounded, ordered, thread-safe lane queue.
//
// CLEAN-ROOM: designed from the server SAD only — §6.1 (the existing world-
// thread <-> IO-worker MPSC queue, which this generalizes: std::mutex +
// condition_variable + deque, drained at tick boundaries), §6.3 (bounded lanes
// + credit-style backpressure: "sends are enqueue-or-shed, never wait" for map-
// thread code; a slow consumer must not stall a producer without a policy),
// §2.2 (the shed order: a full lane sheds per policy). No GPL source consulted.
//
// A `LaneQueue<T>` is the transport primitive under the pub/sub Bus (the enum
// `Lane` in message.hpp names the lane KIND; this is the bounded queue that
// backs one). It is:
//   - TYPED     — carries exactly one message type T (message.hpp identity),
//   - ORDERED   — FIFO within the lane (per-lane ordered delivery, SAD §2.6),
//   - BOUNDED   — a fixed capacity; a full lane triggers the backpressure
//                 policy rather than growing without bound (SAD §6.3),
//   - MPSC/MPMC — many producers, one-or-more consumers, mutex-guarded (the
//                 §6.1 pattern generalized; lock-appropriate for the tick-
//                 boundary drain cadence, not a per-frame hot path).
//
// Backpressure is the crux of §6.3. `push` returns a PushResult so the caller
// (a subsystem's send site) can react — the architecture rule "no map-thread
// code may block on bus.send" is honoured by the non-blocking policies
// (kDropNewest / kDropOldest); kBlock exists for off-tick producers (IO
// workers filling an inbound lane) that MAY wait.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace meridian::bus {

// What a lane does when a producer pushes into a FULL lane (SAD §6.3 / §2.2).
enum class Overflow : std::uint8_t {
    // Block the producer until space frees up (or stop() is called). ONLY for
    // producers that are allowed to wait — never the map/tick thread. Models the
    // credit-exhausted sender that stops at zero (§6.3) for the inbound-IO path.
    kBlock = 0,
    // Drop the message being pushed; keep the queued ones. "enqueue-or-shed,
    // never wait" (§6.3) — the map-thread-safe default. Ordering of survivors
    // is preserved.
    kDropNewest = 1,
    // Evict the OLDEST queued message to make room for the new one. Keeps the
    // freshest data flowing (e.g. latest position wins) — the §2.2 AoI-shed
    // spirit. Ordering of survivors is preserved.
    kDropOldest = 2,
};

// The outcome of a push — lets the send site account for backpressure (the SAD
// wants depth + shed metrics, §8.5). Never silently swallowed.
enum class PushResult : std::uint8_t {
    kEnqueued = 0,     // message is now queued
    kDroppedNewest,    // lane was full; THIS message was dropped (kDropNewest)
    kEvictedOldest,    // lane was full; oldest evicted, this message queued
    kClosed,           // lane stopped (shutting down) — message not queued
};

template <typename T>
class LaneQueue {
public:
    // Build a lane with a fixed capacity and an overflow policy. capacity == 0
    // is treated as 1 (a lane must hold at least one message).
    LaneQueue(std::size_t capacity, Overflow policy)
        : capacity_(capacity == 0 ? 1 : capacity), policy_(policy) {}

    LaneQueue(const LaneQueue&) = delete;
    LaneQueue& operator=(const LaneQueue&) = delete;

    // Push one message. Thread-safe (MP). Applies the overflow policy when full.
    // kBlock waits until space frees or stop() is called (then kClosed). The
    // non-blocking policies never wait — safe from the tick thread.
    PushResult push(T msg) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (stopped_) {
            return PushResult::kClosed;
        }
        if (q_.size() >= capacity_) {
            switch (policy_) {
                case Overflow::kBlock:
                    not_full_.wait(lk, [&] { return stopped_ || q_.size() < capacity_; });
                    if (stopped_) {
                        return PushResult::kClosed;
                    }
                    break;  // space now available; fall through to enqueue
                case Overflow::kDropNewest:
                    ++dropped_;
                    return PushResult::kDroppedNewest;
                case Overflow::kDropOldest:
                    q_.pop_front();
                    ++evicted_;
                    q_.push_back(std::move(msg));
                    not_empty_.notify_one();
                    return PushResult::kEvictedOldest;
            }
        }
        q_.push_back(std::move(msg));
        not_empty_.notify_one();
        return PushResult::kEnqueued;
    }

    // Pop one message, blocking until one is available or the lane is stopped.
    // Returns nullopt only when stopped AND drained (clean shutdown). SC/MC —
    // multiple consumers are supported (each message goes to exactly one).
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(mtx_);
        not_empty_.wait(lk, [&] { return stopped_ || !q_.empty(); });
        if (q_.empty()) {
            return std::nullopt;  // stopped and drained
        }
        T msg = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return msg;
    }

    // Non-blocking pop: nullopt if empty right now. For a tick-boundary drainer
    // that must not stall the world thread.
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty()) {
            return std::nullopt;
        }
        T msg = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return msg;
    }

    // Drain everything currently queued in one shot — the SAD's "drain inbound
    // at the tick boundary" (§3.2). Preserves FIFO order. Non-blocking.
    std::vector<T> drain() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<T> out;
        out.reserve(q_.size());
        while (!q_.empty()) {
            out.push_back(std::move(q_.front()));
            q_.pop_front();
        }
        if (!out.empty()) {
            not_full_.notify_all();
        }
        return out;
    }

    // Stop the lane: wakes all blocked producers/consumers. After stop(), push
    // returns kClosed and pop returns nullopt once drained. Idempotent.
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stopped_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t capacity() const noexcept { return capacity_; }
    Overflow policy() const noexcept { return policy_; }

    // Current depth (SAD §8.5 lane-depth metric input). Momentary under load.
    std::size_t depth() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

    // Backpressure accounting (SAD §8.5 shed counters). Cumulative.
    std::uint64_t dropped() const noexcept { return dropped_.load(); }
    std::uint64_t evicted() const noexcept { return evicted_.load(); }

private:
    const std::size_t capacity_;
    const Overflow policy_;

    mutable std::mutex mtx_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> q_;
    bool stopped_ = false;

    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> evicted_{0};
};

}  // namespace meridian::bus
