# SPDX-License-Identifier: Apache-2.0
#
# libmeridian-bus — the internal message bus (server SAD §2.6, §6.1, §6.3).
#
# Typed publish/subscribe over bounded, ordered, backpressured lanes: the single
# mechanism for cross-subsystem messaging so subsystems never call each other
# synchronously from the update loop. Generalizes the existing world-thread <->
# IO-worker MPSC queue (worldd #82/#87) into a reusable lib that the worker pool
# and future sharding stand on (SAD §7 "M0/M1 unchanged" — the bus rule makes
# M2/M3 transport swaps, not rewrites).
#
# Clean-room (CONTRIBUTING.md): implemented from the written SAD only. No GPL
# source (CMaNGOS / TrinityCore or otherwise) was consulted. Dependency-free by
# design — std threads/mutex/condition_variable only (like libmeridian-core).

add_library(meridian-bus STATIC
  src/message.cpp
)

# Canonical alias — consumers link `meridian::bus` (matches core/net/metrics).
add_library(meridian::bus ALIAS meridian-bus)

target_include_directories(meridian-bus
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(meridian-bus PUBLIC cxx_std_20)

set_target_properties(meridian-bus PROPERTIES
  CXX_EXTENSIONS OFF
)

# The bus lib is hand-written game infrastructure, so hold IT (and its tests) to
# -Werror / zero warnings even while the root tree stays -Wall/-Wextra without
# -Werror at the M0 skeleton stage. Scoped to these targets only — this does NOT
# tighten sibling subdirs (net/db/proto pull third-party headers that may warn).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(MERIDIAN_BUS_WARNINGS -Wall -Wextra -Wpedantic -Werror)
else()
  set(MERIDIAN_BUS_WARNINGS "")
endif()
target_compile_options(meridian-bus PRIVATE ${MERIDIAN_BUS_WARNINGS})

# --- Unit test: pub/sub, ordering, multi-subscriber, backpressure, threads. ---
# Pure (no DB, no socket) — runs in the plain `server` CI job's ctest. Covers:
#   * publish/subscribe delivers TYPED messages in FIFO order,
#   * multiple subscribers each receive every message,
#   * a bounded lane applies backpressure when full (drop-newest / drop-oldest /
#     block per policy),
#   * thread-safety: concurrent producers + consumers, no loss / no interleave.
find_package(Threads REQUIRED)
add_executable(meridian-bus-test test/bus_test.cpp)
target_link_libraries(meridian-bus-test PRIVATE meridian::bus Threads::Threads)
target_compile_options(meridian-bus-test PRIVATE ${MERIDIAN_BUS_WARNINGS})
set_target_properties(meridian-bus-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME bus-unit COMMAND meridian-bus-test)

# --- Architecture test: the "bus-only" discipline (SAD §2.6 CI rule). ---------
# Proves two subsystems interact ONLY via the bus (a producer publishes, a
# consumer receives, with NO direct call between them), that ordering holds,
# that backpressure engages when a lane fills, and the structural invariant that
# the bus API leaks no subscriber internals across the bus (typed values only).
# This is the in-process form of the SAD's "the CI architecture test still fails
# any cross-map reach that bypasses [the bus]" (§2.6). See test/README then.
add_executable(meridian-bus-architecture-test test/architecture_test.cpp)
target_link_libraries(meridian-bus-architecture-test PRIVATE meridian::bus Threads::Threads)
target_compile_options(meridian-bus-architecture-test PRIVATE ${MERIDIAN_BUS_WARNINGS})
set_target_properties(meridian-bus-architecture-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME bus-architecture COMMAND meridian-bus-architecture-test)
