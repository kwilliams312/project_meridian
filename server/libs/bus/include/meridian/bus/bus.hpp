// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-bus — the message bus: typed publish / subscribe over bounded,
// ordered, backpressured lanes (server SAD §2.6, §6.1, §6.3).
//
// CLEAN-ROOM: designed from the server SAD only. No GPL source (CMaNGOS /
// TrinityCore or otherwise) was consulted. See CONTRIBUTING.md.
//
// ── WHAT THIS IS ──────────────────────────────────────────────────────────
// The single mechanism for cross-subsystem messaging (SAD §2.6). Subsystems do
// NOT call each other synchronously from the update loop; they publish TYPED
// messages onto a lane and other subsystems subscribe to receive them. This is
// the decoupling that lets the world thread hand work to IO/DB workers (§6.1),
// enables the worker pool + future sharding (§7), and — because the API is the
// same in-process as it will be over the M2 mesh — makes the transport swap a
// swap, not a rewrite (§7 "M0/M1 unchanged").
//
// ── THE MODEL ─────────────────────────────────────────────────────────────
//   publish<T>(lane, msg)          — enqueue a copy of `msg` to every
//                                    subscription registered for {type(T), lane}.
//                                    Returns the WORST push result across those
//                                    subscriptions (so a full lane surfaces).
//                                    Non-blocking for the tick thread when the
//                                    lane's policy is drop-based (§6.3).
//   subscribe<T>(lane, capacity,   — register interest in {T, lane}; returns a
//                policy)             Subscription<T> — the subscriber's own
//                                    bounded inbox (an MPSC lane, §6.1). Each
//                                    subscription is an independent lane, so one
//                                    slow subscriber backpressures ITSELF, never
//                                    the publisher's other subscribers.
//
// A subscriber drains its inbox at ITS cadence (the world thread at a tick
// boundary, a worker in its loop). Ordering holds per subscription (per-lane
// FIFO, SAD §2.6). Delivery is at-most-once per subscription; a full inbox
// sheds per that subscription's policy (SAD §6.3) — the bus is never a
// durability layer (§5.6: "anything needing stronger guarantees goes through
// the DB").
//
// ── THE DISCIPLINE (what the architecture test enforces) ──────────────────
// "Bus-only" means: subsystem A affects subsystem B ONLY by publishing a typed
// message B subscribed to. The Bus API exposes NO handle to any subscriber's
// state — publish takes a type + a value, never a pointer to B; a Subscription
// hands back only VALUES of T, never a reference into the producer. So there is
// no API path for A to reach into B's internals across the bus. See
// test/architecture_test.cpp and README.md.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "meridian/bus/lane.hpp"
#include "meridian/bus/message.hpp"

namespace meridian::bus {

// A subscriber's inbox for messages of type T on one lane. Handed back from
// Bus::subscribe. The ONLY things it exposes are VALUES of T (drain / pop /
// try_pop) and its own backpressure accounting — never a handle back into the
// publisher (the bus-only invariant). Move-only; owns a shared lane the bus
// publishes into.
template <typename T>
class Subscription {
public:
    // Drain everything queued right now (tick-boundary drain, SAD §3.2). FIFO.
    std::vector<T> drain() { return lane_->drain(); }

    // Blocking single pop (worker-loop style). nullopt when the bus stops and
    // this inbox is drained.
    std::optional<T> pop() { return lane_->pop(); }

    // Non-blocking single pop. nullopt if empty right now.
    std::optional<T> try_pop() { return lane_->try_pop(); }

    Lane lane_kind() const noexcept { return kind_; }
    std::size_t depth() const { return lane_->depth(); }
    std::uint64_t dropped() const noexcept { return lane_->dropped(); }
    std::uint64_t evicted() const noexcept { return lane_->evicted(); }

private:
    friend class Bus;
    Subscription(std::shared_ptr<meridian::bus::LaneQueue<T>> lane, Lane kind)
        : lane_(std::move(lane)), kind_(kind) {}

    std::shared_ptr<meridian::bus::LaneQueue<T>> lane_;
    Lane kind_;
};

// The message bus. One instance is shared (thread-safe) across all subsystems
// in a process. Publishers and subscribers reference only THIS and the message
// types — never each other (bus-only, SAD §2.6).
class Bus {
public:
    Bus() = default;
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    ~Bus() { stop(); }

    // Register interest in messages of type T on `lane`. Returns a Subscription
    // with its own bounded inbox (capacity + overflow policy per SAD §6.3).
    // Thread-safe against concurrent publish/subscribe. May be called before or
    // after publishers exist; a message published before a subscriber exists is
    // simply not delivered to it (pub/sub, not a durable queue — SAD §5.6).
    template <typename T>
    Subscription<T> subscribe(Lane lane, std::size_t capacity, Overflow policy) {
        auto inbox = std::make_shared<meridian::bus::LaneQueue<T>>(capacity, policy);
        const Key key{message_type_id<T>(), lane};
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto& subs = table_[key];
            // Store a type-erased fan-out closure + a stopper, so publish<T> and
            // stop() work without knowing T at those sites. The closure captures
            // the concrete LaneQueue<T> and is only ever invoked from publish<T> with
            // a matching T (the Key guarantees the type), so the void* cast back
            // is type-safe by construction.
            SubEntry entry;
            entry.stop = [inbox] { inbox->stop(); };
            entry.deliver = [inbox](const void* msg) -> PushResult {
                // msg points at a `const T` supplied by publish<T> for the SAME
                // T (matched by Key). Copy it into this subscriber's inbox — each
                // subscription gets its own copy (independent backpressure).
                return inbox->push(*static_cast<const T*>(msg));
            };
            subs.push_back(std::move(entry));
        }
        return Subscription<T>(std::move(inbox), lane);
    }

    // Publish `msg` to every subscription for {type(T), lane}. Returns the WORST
    // PushResult across them (kEnqueued if all fine; a drop/evict/closed if any
    // lane pushed back) so the send site can account for backpressure — the SAD
    // wants shed visibility (§8.5), never a silent swallow. When there are no
    // subscribers the message is dropped (kEnqueued is returned: nothing pushed
    // back). Thread-safe; non-blocking for drop-policy lanes (tick-safe, §6.3).
    template <typename T>
    PushResult publish(Lane lane, const T& msg) {
        const Key key{message_type_id<T>(), lane};
        // Snapshot the deliver closures under the lock, then invoke them without
        // it — a subscriber's push (which may block on a kBlock lane) must never
        // hold the bus table lock (that would serialize the whole bus). The
        // shared_ptr captured in each closure keeps the inbox alive across the
        // unlocked call even if the Subscription is dropped concurrently.
        std::vector<Deliver> targets;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = table_.find(key);
            if (it == table_.end()) {
                return PushResult::kEnqueued;  // no subscribers: nothing to shed
            }
            targets.reserve(it->second.size());
            for (const auto& e : it->second) {
                targets.push_back(e.deliver);
            }
        }
        PushResult worst = PushResult::kEnqueued;
        for (const auto& deliver : targets) {
            const PushResult r = deliver(static_cast<const void*>(&msg));
            worst = worse_of(worst, r);
        }
        return worst;
    }

    // Number of live subscriptions for {type(T), lane} — test/diagnostic.
    template <typename T>
    std::size_t subscriber_count(Lane lane) {
        const Key key{message_type_id<T>(), lane};
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = table_.find(key);
        return it == table_.end() ? 0 : it->second.size();
    }

    // Stop every lane: wakes all blocked producers/consumers so subscriber
    // threads drain and exit. Idempotent; also called by the dtor.
    void stop() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [key, subs] : table_) {
            (void)key;
            for (auto& e : subs) {
                e.stop();
            }
        }
    }

private:
    // Routing key: a message type on a lane. Two subscribers for the same
    // {type, lane} both receive each publish; different types/lanes are isolated.
    struct Key {
        MessageTypeId type;
        Lane lane;
        bool operator==(const Key& o) const noexcept {
            return type == o.type && lane == o.lane;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            // Mix the u32 type id with the u8 lane. Distinct lanes of a type land
            // in distinct buckets; good enough for a small per-process table.
            return (static_cast<std::size_t>(k.type) << 8) ^
                   static_cast<std::size_t>(static_cast<std::uint8_t>(k.lane));
        }
    };

    using Deliver = std::function<PushResult(const void*)>;
    struct SubEntry {
        Deliver deliver;              // push a *const T into this subscriber's inbox
        std::function<void()> stop;   // stop this subscriber's inbox
    };

    // Ordering of PushResult severity for "worst across subscribers": a closed
    // or dropped lane is worse than an evict, which is worse than a clean
    // enqueue. Lets publish return a single meaningful backpressure signal.
    static PushResult worse_of(PushResult a, PushResult b) noexcept {
        auto rank = [](PushResult r) -> int {
            switch (r) {
                case PushResult::kEnqueued:      return 0;
                case PushResult::kEvictedOldest: return 1;
                case PushResult::kDroppedNewest: return 2;
                case PushResult::kClosed:        return 3;
            }
            return 0;
        };
        return rank(a) >= rank(b) ? a : b;
    }

    std::mutex mtx_;
    std::unordered_map<Key, std::vector<SubEntry>, KeyHash> table_;
};

}  // namespace meridian::bus
