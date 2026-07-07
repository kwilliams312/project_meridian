# SPDX-License-Identifier: Apache-2.0
#
# telemetryd — client telemetry ingest daemon (OPS-05 / D-29; issue #167).
#
# The SERVER half of the D-29 client-telemetry loop: it RECEIVES the #168 client
# ERROR/CRITICAL log batches (+ crash / missing-content events), validates them
# against the privacy contract, rate-limits per build/IP, and forwards validated
# events to the structured-log sink. Split as a library (telemetryd-ingest: the
# pure parse/validate/rate-limit core + the HTTP server) + the daemon + tests —
# mirroring authd/worldd so the tests drive the exact ingest path the daemon runs.
#
# Composes:
#   meridian::core     — logging / version (#68)
#   meridian::metrics  — /metrics exposer + the client-ingest counters (#164)
# Clean-room from the SAD / privacy contract; no GPL and no HTTP/JSON library
# consulted (hand-rolled JSON reader + RFC 7230 responder). See CONTRIBUTING.md.

find_package(Threads REQUIRED)

# --- Ingest core (pure) + HTTP server as a library. ---------------------------
add_library(telemetryd-ingest STATIC
  ingest.cpp
  ingest_http.cpp
)
add_library(meridian::telemetryd-ingest ALIAS telemetryd-ingest)
target_include_directories(telemetryd-ingest
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(telemetryd-ingest
  PUBLIC
    meridian::metrics  # OPS-05 client-ingest counters + /metrics registry (#164)
    Threads::Threads
)
target_compile_features(telemetryd-ingest PUBLIC cxx_std_20)
set_target_properties(telemetryd-ingest PROPERTIES CXX_EXTENSIONS OFF)

# --- The daemon. --------------------------------------------------------------
add_executable(telemetryd
  main.cpp
)
target_link_libraries(telemetryd
  PRIVATE
    meridian::core
    meridian::telemetryd-ingest
    Threads::Threads
)
set_target_properties(telemetryd PROPERTIES CXX_EXTENSIONS OFF)

# --- Unit test: parse / validate / rate-limit / sink as PURE functions. -------
# No socket, no DB, no network — exercises the whole D-29 contract directly:
# a valid #168 envelope parses + validates + forwards; malformed / oversized /
# bad-type / bad-level are rejected with the exact reason; a PII-shaped field or
# unknown tag is rejected; the rate limiter caps a flood. Runs in the plain
# `server` job's ctest.
add_executable(telemetryd-ingest-test test/ingest_test.cpp)
target_link_libraries(telemetryd-ingest-test PRIVATE meridian::telemetryd-ingest)
set_target_properties(telemetryd-ingest-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME telemetryd-ingest COMMAND telemetryd-ingest-test)

# --- Integration test: the ingest HTTP endpoint (in-process, no DB). ----------
# Stands the real IngestServer on an ephemeral loopback port, drives raw HTTP/1.1
# POSTs over a BSD socket, and asserts: a valid #168 batch → 200 + forwarded to
# the sink; malformed/oversized → 4xx with no crash; a PII field → 422; a flood →
# 429 after the cap. Self-contained (loopback only), so it runs DB-free in the
# `server` CI job's ctest.
add_executable(telemetryd-http-test test/ingest_http_test.cpp)
target_link_libraries(telemetryd-http-test PRIVATE meridian::telemetryd-ingest Threads::Threads)
set_target_properties(telemetryd-http-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME telemetryd-http COMMAND telemetryd-http-test)
