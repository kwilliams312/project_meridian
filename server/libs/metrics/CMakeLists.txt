# SPDX-License-Identifier: Apache-2.0
#
# libmeridian-metrics — the OPS-05 in-process Prometheus metric registry + the
# /metrics HTTP exposer (server SAD §8.5; docs/telemetry-architecture.md §5;
# D-29 §9). Clean-room from the Prometheus exposition-format spec + BSD sockets;
# no third-party dependency (no prometheus-cpp / civetweb) — dependency-light by
# design, like meridian::core (CONTRIBUTING.md).

find_package(Threads REQUIRED)

add_library(meridian-metrics STATIC
  src/registry.cpp
  src/exposer.cpp
)
add_library(meridian::metrics ALIAS meridian-metrics)

target_include_directories(meridian-metrics
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(meridian-metrics PUBLIC Threads::Threads)
target_compile_features(meridian-metrics PUBLIC cxx_std_20)
set_target_properties(meridian-metrics PROPERTIES CXX_EXTENSIONS OFF)

# --- Unit test: registry + exposition format (no socket, always runs). --------
# Counter increments, gauge set/inc/dec, histogram bucketing + cumulative render,
# label arity, and that render() emits valid Prometheus exposition text with the
# expected HELP/TYPE headers and _bucket/_sum/_count triple. Pure — runs in the
# plain server ctest (no DB, no network).
add_executable(meridian-metrics-test test/registry_test.cpp)
target_link_libraries(meridian-metrics-test PRIVATE meridian::metrics)
set_target_properties(meridian-metrics-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME metrics-registry COMMAND meridian-metrics-test)

# --- Integration test: the /metrics HTTP endpoint (in-process, no DB). ---------
# Stands the real Exposer on an ephemeral loopback port, drives a raw HTTP/1.1
# GET over a BSD socket, and asserts a 200 with valid exposition text carrying
# catalog metric names — AND that a metric MOVES when its event fires (bump a
# counter, re-scrape, assert the new value). Self-contained (loopback only), so
# it runs DB-free in the `server` CI job's ctest.
add_executable(meridian-metrics-exposer-test test/exposer_test.cpp)
target_link_libraries(meridian-metrics-exposer-test PRIVATE meridian::metrics Threads::Threads)
set_target_properties(meridian-metrics-exposer-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME metrics-exposer COMMAND meridian-metrics-exposer-test)

enable_testing()
