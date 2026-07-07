# SPDX-License-Identifier: Apache-2.0
#
# libmeridian-trace — the OPS-05 session-flow tracing lib: a clean-room span model
# + a hand-rolled OTLP/HTTP-JSON exporter (issue #166; D-29 §9 rule 5;
# docs/telemetry-architecture.md §5.3). This is the TRACES leg of the OPS-05
# triple alongside metrics (#164) and logs (#165).
#
# CLEAN-ROOM, dependency-light (CONTRIBUTING.md): built from the PUBLIC OTLP trace
# data model + its JSON/HTTP binding and the W3C Trace-Context id shapes only —
# no OTel C++ SDK, no protobuf/gRPC, no third-party HTTP/JSON library. The OTLP/
# JSON serializer + the HTTP/1.1 POST client are hand-rolled (the latter mirrors
# meridian::metrics' hand-rolled HTTP/1.1 SERVER). Only Threads (std::thread for
# the async exporter) is linked — like meridian::metrics.

find_package(Threads REQUIRED)

add_library(meridian-trace STATIC
  src/span.cpp
  src/tracer.cpp
  src/otlp.cpp
  src/exporter.cpp
  src/session_flow.cpp
)
add_library(meridian::trace ALIAS meridian-trace)

target_include_directories(meridian-trace
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(meridian-trace PUBLIC Threads::Threads)
target_compile_features(meridian-trace PUBLIC cxx_std_20)
set_target_properties(meridian-trace PROPERTIES CXX_EXTENSIONS OFF)

# --- Unit test: OTLP/JSON serialization + exporter batching (pure, no socket). --
# Asserts:
#   1. a span serializes to valid OTLP/HTTP JSON — parse it back, check
#      traceId/spanId/name/kind/attributes/status/timestamps,
#   2. a parent/child span pair is well-formed (shared traceId, child.parentSpanId
#      == parent.spanId, distinct spanIds),
#   3. the grant→trace stitching is deterministic + agrees across the two hops,
#   4. the async exporter BATCHES finished spans and delivers OTLP bodies to a MOCK
#      sink (no live collector) — assert the mock received the spans,
#   5. an UNCONFIGURED exporter is a no-op (drops spans, exports nothing),
#   6. endpoint normalization (base -> .../v1/traces).
# Pure/in-process: uses an injected sink, opens no socket, needs no collector — so
# it runs in the plain `server` CI job's ctest (like the metrics registry test).
# A minimal hand-rolled JSON scanner (no dependency) parses the bodies back.
add_executable(meridian-trace-test test/trace_test.cpp)
target_link_libraries(meridian-trace-test PRIVATE meridian::trace Threads::Threads)
set_target_properties(meridian-trace-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME trace-otlp COMMAND meridian-trace-test)

enable_testing()
