<!-- SPDX-License-Identifier: Apache-2.0 -->
# libmeridian-bus — the "bus-only" architecture test

This directory holds the message-bus tests (issue #88). Two executables run in
the plain `server` CI job's `ctest`:

| ctest name        | executable                        | what it proves |
|-------------------|-----------------------------------|----------------|
| `bus-unit`        | `bus_test.cpp`                    | pub/sub delivers typed messages in order; multiple subscribers each receive; bounded lanes apply backpressure (drop-newest / drop-oldest / block per policy); thread-safe MPSC (no loss, no torn/duplicate messages under concurrent producers). |
| `bus-architecture`| `architecture_test.cpp`           | the **bus-only discipline** (below). |

## What the bus is (server SAD §2.6, §6.1, §6.3)

The internal message bus is *the single mechanism for cross-subsystem
messaging*. Server subsystems (the world/update thread, DB workers, the AoI
engine, the future chat/mail/AH managers) do **not** call each other
synchronously from the update loop. They **publish typed messages onto a lane**;
other subsystems **subscribe** and drain their own inbox at their own cadence.
This decoupling is what lets the world thread hand work to the IO/DB worker pool
(§6.1) and what makes the M2 gateway split + M3 sharding *transport swaps rather
than rewrites* (§7: "M0/M1 unchanged").

The v0 lib (`server/libs/bus/`) provides:

- **Typed publish/subscribe** — `bus.publish<T>(lane, msg)` /
  `bus.subscribe<T>(lane, capacity, policy)`. Routing is keyed on the message
  **type** (`message_type_id<T>()`, the in-process stand-in for the SAD §5.6
  envelope's `payload_type`) **and** the lane. A subscriber for `T` on a lane
  only ever receives `T` on that lane.
- **Lanes** — `kControl`, `kSession`, `kBulk` (SAD §5.6/§6.3). Each lane is
  independently ordered and independently backpressured; the control lane's
  isolation is expressed as its own bounded inbox that a saturated data lane
  cannot touch.
- **Bounded lanes + backpressure** (SAD §6.3) — each subscription is a bounded
  MPSC queue with an `Overflow` policy: `kBlock` (producers allowed to wait —
  the IO-inbound path), `kDropNewest` and `kDropOldest` (non-blocking, the
  tick-thread-safe policies: "sends are enqueue-or-shed, never wait"). Shed and
  eviction are **counted** (`dropped()`, `evicted()`) for the SAD §8.5 lane-depth
  / credit-stall metrics — never silently swallowed.
- **Ordered per-lane delivery** — FIFO within a subscription.

The transport generalizes the existing world-thread ↔ IO-worker MPSC queue in
`server/worldd/world_dispatch.cpp` (#82/#87: `std::mutex` +
`condition_variable` + `std::deque`, drained at the tick boundary) into a
reusable, typed, multi-lane lib. It is dependency-free (std threads only), like
`libmeridian-core`.

## What "bus-only" means

Subsystem **A** affects subsystem **B** *bus-only* iff:

1. **A publishes; it never calls B.** A holds no `B*`, names no B method, and
   touches no B field. Its only cross-subsystem reference is the shared `Bus`.
2. **B reacts by draining its own inbox** — never by A reaching into it.
3. **The bus carries typed, self-contained values.** A subscriber receives
   *copies of `T`*, never a handle back into the producer. `publish` returns a
   plain `PushResult` enum (a backpressure signal), never a pointer to a
   subscriber; `Subscription` yields `std::vector<T>` / `std::optional<T>`.
4. **A full lane applies backpressure per policy** — bounded, never unbounded
   growth, never a silent stall; the tick-path producer never blocks.
5. **Per-lane delivery is ordered.**

## How `architecture_test.cpp` enforces it

- **Separate, mutually-invisible subsystems.** `subsystem_a::WorldThread`
  (producer) and `subsystem_b::DbWorker` (consumer) live in different
  namespaces. Neither includes or names the other; each can see only `Bus` and
  the shared message type `contract::SaveRequest`. They are wired together in
  `main()` through the bus alone. **If someone made `WorldThread` call
  `DbWorker` directly, it would not compile** — the enforcement is the absence
  of any handle, made deliberate by construction.
- **Compile-time API-surface asserts** (`static_assert`) pin the shapes that
  keep the bus from leaking internals: `publish<T>` returns `PushResult` (a
  value, not a subscriber handle); `Subscription::drain/try_pop` yield *values*
  of `T`; a bus message is trivially-copyable (carries no back-door pointer). A
  future change that tried to leak a subscriber handle across the bus (e.g.
  `publish` returning a `B*`) **fails the build**. This is the mechanical core
  of the SAD's "the CI architecture test fails any cross-subsystem reach that
  bypasses the bus" (§2.6).
- **Runtime invariants:** bus-only delivery (A produces, B consumes, no direct
  call; B's state is untouched until *it* drains), per-lane ordering,
  backpressure engaging on a full lane *without blocking the producer* (a
  single-threaded over-publish that would deadlock if the tick-path send could
  block), and all of the above holding under concurrency (producer and consumer
  on separate threads, no loss beyond policy, coherent typed values).

Both tests build under `-Werror -Wall -Wextra -Wpedantic` (scoped to the bus
targets in `server/libs/bus/CMakeLists.txt`).

## worldd migration — a documented follow-up (out of v0 scope)

v0 is the bus lib + its tests. Migrating worldd's existing world-thread ↔
IO-worker queue (`WorldServer::enqueue` / `WorldEvent`, `world_dispatch.cpp`)
onto `meridian::bus` is intentionally **deferred** so this change does not
destabilise #82/#87. The seam is clean when it happens:

- `WorldEvent` becomes a bus message type; the inbound MPSC deque becomes a
  `kSession`-lane `Subscription<WorldEvent>` drained in `world_thread_main()`'s
  tick body (the current `batch.swap` drain maps directly onto
  `Subscription::drain()`).
- The AoI relay's cross-session sends (#87) publish typed AoI/movement messages
  rather than touching another session's egress directly — bringing worldd under
  the same `bus-architecture` rule this test establishes.
- No wire/protocol change: the bus API is identical in-process now and over the
  M2 mesh later (SAD §5.6/§7); only the transport under `publish`/`drain` is
  swapped.
