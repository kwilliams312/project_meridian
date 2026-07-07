// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-bus unit test (#88) — pub/sub, ordering, multi-subscriber,
// backpressure policies, and thread-safety.
//
// CLEAN-ROOM: exercises the SAD §2.6/§6.1/§6.3 behaviours only. Pure — no DB, no
// socket — so it runs in the plain `server` CI job's ctest. Self-contained
// assertion harness (no gtest dep), matching the repo's other pure lib tests.

#include "meridian/bus/bus.hpp"
#include "meridian/bus/lane.hpp"
#include "meridian/bus/message.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace meridian::bus;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// ── Test message types (distinct C++ types => distinct routing ids). ─────────
struct MoveMsg {
    std::uint64_t session = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
};
struct ChatMsg {
    std::uint64_t from = 0;
    std::string text;
};

// ── message_type_id: stable per type, distinct across types. ─────────────────
void test_type_ids_are_stable_and_distinct() {
    const MessageTypeId a1 = message_type_id<MoveMsg>();
    const MessageTypeId a2 = message_type_id<MoveMsg>();
    const MessageTypeId b1 = message_type_id<ChatMsg>();
    check(a1 == a2, "message_type_id stable for same type");
    check(a1 != b1, "message_type_id distinct across types");
    check(a1 != 0 && b1 != 0, "message_type_id never the reserved 0");
}

// ── publish/subscribe delivers TYPED messages in FIFO order. ─────────────────
void test_publish_delivers_typed_in_order() {
    Bus bus;
    auto sub = bus.subscribe<MoveMsg>(Lane::kSession, /*capacity=*/16, Overflow::kBlock);
    check(bus.subscriber_count<MoveMsg>(Lane::kSession) == 1, "one subscriber registered");

    for (std::int32_t i = 0; i < 5; ++i) {
        const PushResult r = bus.publish(Lane::kSession, MoveMsg{1, i, i * 2});
        check(r == PushResult::kEnqueued, "publish enqueued (lane not full)");
    }

    const std::vector<MoveMsg> got = sub.drain();
    check(got.size() == 5, "drained all 5 messages");
    bool ordered = true;
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(got.size()); ++i) {
        if (got[static_cast<std::size_t>(i)].x != i ||
            got[static_cast<std::size_t>(i)].y != i * 2) {
            ordered = false;
        }
    }
    check(ordered, "messages delivered in FIFO order with correct typed fields");
}

// ── lane isolation: a subscriber on one lane does NOT see another lane. ───────
void test_lanes_are_isolated() {
    Bus bus;
    auto session_sub = bus.subscribe<MoveMsg>(Lane::kSession, 8, Overflow::kBlock);
    auto control_sub = bus.subscribe<MoveMsg>(Lane::kControl, 8, Overflow::kBlock);

    bus.publish(Lane::kSession, MoveMsg{1, 10, 10});

    check(session_sub.drain().size() == 1, "session lane received its message");
    check(control_sub.drain().empty(), "control lane isolated — got nothing");
}

// ── type isolation: a ChatMsg subscriber does NOT receive MoveMsg. ───────────
void test_types_are_isolated() {
    Bus bus;
    auto move_sub = bus.subscribe<MoveMsg>(Lane::kSession, 8, Overflow::kBlock);
    auto chat_sub = bus.subscribe<ChatMsg>(Lane::kSession, 8, Overflow::kBlock);

    bus.publish(Lane::kSession, MoveMsg{1, 3, 4});

    check(move_sub.drain().size() == 1, "MoveMsg subscriber received MoveMsg");
    check(chat_sub.drain().empty(), "ChatMsg subscriber isolated — no MoveMsg leaked");
}

// ── multiple subscribers: each independent inbox receives every message. ─────
void test_multiple_subscribers_each_receive() {
    Bus bus;
    auto s1 = bus.subscribe<ChatMsg>(Lane::kBulk, 16, Overflow::kBlock);
    auto s2 = bus.subscribe<ChatMsg>(Lane::kBulk, 16, Overflow::kBlock);
    auto s3 = bus.subscribe<ChatMsg>(Lane::kBulk, 16, Overflow::kBlock);
    check(bus.subscriber_count<ChatMsg>(Lane::kBulk) == 3, "three subscribers registered");

    bus.publish(Lane::kBulk, ChatMsg{7, "hello"});

    check(s1.drain().size() == 1, "subscriber 1 received the fan-out");
    check(s2.drain().size() == 1, "subscriber 2 received the fan-out");
    check(s3.drain().size() == 1, "subscriber 3 received the fan-out");
}

// ── backpressure: kDropNewest sheds the new message when full. ───────────────
void test_backpressure_drop_newest() {
    Bus bus;
    auto sub = bus.subscribe<MoveMsg>(Lane::kSession, /*capacity=*/2, Overflow::kDropNewest);

    check(bus.publish(Lane::kSession, MoveMsg{1, 0, 0}) == PushResult::kEnqueued, "1st fits");
    check(bus.publish(Lane::kSession, MoveMsg{1, 1, 0}) == PushResult::kEnqueued, "2nd fits");
    // Lane now full (cap 2). Third push is shed, NOT blocked (tick-safe).
    check(bus.publish(Lane::kSession, MoveMsg{1, 2, 0}) == PushResult::kDroppedNewest,
          "3rd dropped (drop-newest, non-blocking)");

    const std::vector<MoveMsg> got = sub.drain();
    check(got.size() == 2, "only 2 survived under drop-newest");
    check(got[0].x == 0 && got[1].x == 1, "survivors are the OLDEST two, in order");
    check(sub.dropped() == 1, "dropped counter recorded the shed");
}

// ── backpressure: kDropOldest evicts the oldest, keeps the freshest. ─────────
void test_backpressure_drop_oldest() {
    Bus bus;
    auto sub = bus.subscribe<MoveMsg>(Lane::kSession, /*capacity=*/2, Overflow::kDropOldest);

    bus.publish(Lane::kSession, MoveMsg{1, 0, 0});
    bus.publish(Lane::kSession, MoveMsg{1, 1, 0});
    // Full; this evicts x=0 and keeps {1,2}.
    check(bus.publish(Lane::kSession, MoveMsg{1, 2, 0}) == PushResult::kEvictedOldest,
          "3rd evicts oldest (drop-oldest)");

    const std::vector<MoveMsg> got = sub.drain();
    check(got.size() == 2, "still 2 under drop-oldest");
    check(got[0].x == 1 && got[1].x == 2, "survivors are the FRESHEST two, in order");
    check(sub.evicted() == 1, "evicted counter recorded the eviction");
}

// ── backpressure: kBlock makes a producer wait until a consumer drains. ──────
void test_backpressure_block_waits_for_drain() {
    Bus bus;
    auto sub = bus.subscribe<MoveMsg>(Lane::kSession, /*capacity=*/1, Overflow::kBlock);

    check(bus.publish(Lane::kSession, MoveMsg{1, 0, 0}) == PushResult::kEnqueued, "1st fits (cap 1)");

    std::atomic<bool> second_done{false};
    std::thread producer([&] {
        // Lane is full; kBlock => this waits until the consumer pops.
        bus.publish(Lane::kSession, MoveMsg{1, 1, 0});
        second_done.store(true);
    });

    // The producer must still be blocked (no consumer has drained yet). We can't
    // prove "never" without a race, but a short spin should still show blocked.
    for (int i = 0; i < 1000 && !second_done.load(); ++i) {
        std::this_thread::yield();
    }
    check(!second_done.load(), "producer blocked while lane full (kBlock)");

    // Drain one — frees a slot, unblocks the producer.
    const std::vector<MoveMsg> first = sub.drain();
    check(first.size() >= 1, "consumer drained at least the first message");

    producer.join();
    check(second_done.load(), "producer unblocked once a slot freed");
    // The second message is now deliverable.
    const std::vector<MoveMsg> rest = sub.drain();
    check(rest.size() == 1 && rest[0].x == 1, "blocked message delivered after unblock");
}

// ── publish to no subscribers is a clean no-op (pub/sub, not durable). ───────
void test_publish_no_subscribers_is_noop() {
    Bus bus;
    const PushResult r = bus.publish(Lane::kBulk, ChatMsg{1, "into the void"});
    check(r == PushResult::kEnqueued, "publish with zero subscribers returns clean");
}

// ── thread-safety: N producers + 1 consumer, no loss, no interleave/corruption.
void test_thread_safety_mpsc_no_loss() {
    Bus bus;
    // Ample capacity + kBlock => every message is delivered exactly once (no
    // shed). Proves the MPSC path is race-free under concurrent producers.
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 5000;
    constexpr int kTotal = kProducers * kPerProducer;

    auto sub = bus.subscribe<MoveMsg>(Lane::kSession, /*capacity=*/kTotal + 16, Overflow::kBlock);

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&bus, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                // Encode producer id + sequence so the consumer can verify both
                // completeness (every pair present) and no corruption (fields
                // arrive as a coherent unit, never torn).
                bus.publish(Lane::kSession,
                            MoveMsg{static_cast<std::uint64_t>(p), i, p});
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }

    const std::vector<MoveMsg> got = sub.drain();
    check(got.size() == static_cast<std::size_t>(kTotal), "no messages lost under concurrency");

    // Every (producer, seq) pair appears exactly once; fields are internally
    // consistent (session == y means the struct was never torn across threads).
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(static_cast<std::size_t>(kTotal));
    bool coherent = true;
    for (const MoveMsg& m : got) {
        if (m.session != static_cast<std::uint64_t>(m.y)) {
            coherent = false;  // torn write => session and y disagree
        }
        const std::uint64_t pair = (m.session << 32) | static_cast<std::uint32_t>(m.x);
        if (!seen.insert(pair).second) {
            coherent = false;  // duplicate
        }
    }
    check(coherent, "no torn/duplicate messages — each (producer,seq) exactly once");
    check(seen.size() == static_cast<std::size_t>(kTotal), "all distinct pairs present");
}

// ── thread-safety: blocking consumers on a stopped bus drain then exit. ───────
void test_stop_wakes_blocked_consumers() {
    Bus bus;
    auto sub = bus.subscribe<ChatMsg>(Lane::kControl, 8, Overflow::kBlock);

    std::atomic<int> received{0};
    std::atomic<bool> exited{false};
    std::thread consumer([&] {
        // Blocking pop loop — exits only when the bus stops AND the inbox drains.
        while (auto msg = sub.pop()) {
            (void)msg;
            received.fetch_add(1);
        }
        exited.store(true);
    });

    bus.publish(Lane::kControl, ChatMsg{1, "a"});
    bus.publish(Lane::kControl, ChatMsg{1, "b"});

    bus.stop();  // must wake the blocked pop so the consumer can exit
    consumer.join();

    check(exited.load(), "blocked consumer woke and exited after stop()");
    check(received.load() == 2, "consumer drained both messages before exit");
}

}  // namespace

int main() {
    test_type_ids_are_stable_and_distinct();
    test_publish_delivers_typed_in_order();
    test_lanes_are_isolated();
    test_types_are_isolated();
    test_multiple_subscribers_each_receive();
    test_backpressure_drop_newest();
    test_backpressure_drop_oldest();
    test_backpressure_block_waits_for_drain();
    test_publish_no_subscribers_is_noop();
    test_thread_safety_mpsc_no_loss();
    test_stop_wakes_blocked_consumers();

    if (g_failures == 0) {
        std::puts("bus-unit: all checks passed");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "bus-unit: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
