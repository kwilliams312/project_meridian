# SPDX-License-Identifier: Apache-2.0
#
# worldd — shard worker / map simulation daemon (server SAD §2.5; issues #82/#83).
#
# The IF-2 opcode dispatcher + world-process scaffold (world_dispatch) composes
# three merged libs:
#   meridian::core  — logging / version (#68)
#   meridian::net   — TLS 1.3 Session + IF-2 length framing (M0 transport, #77)
#   meridian::proto — world.fbs FlatBuffers codegen (#67)
# Split as a library (world-dispatch) + the daemon (worldd) + an integration
# test, mirroring authd's structure so the test can drive the exact serve/dispatch
# path the daemon runs (not buried in main()). Clean-room from the SAD; no GPL
# source consulted (CONTRIBUTING.md).

find_package(Threads REQUIRED)

# --- Dispatcher + world-process scaffold as a library, so the integration test
#     can stand up the same serve loop the daemon runs. ------------------------
add_library(world-dispatch STATIC
  world_dispatch.cpp
)
add_library(meridian::world-dispatch ALIAS world-dispatch)
target_include_directories(world-dispatch
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(world-dispatch
  PUBLIC
    meridian::core
    meridian::net
    meridian::proto
    Threads::Threads
)
target_compile_features(world-dispatch PUBLIC cxx_std_20)
set_target_properties(world-dispatch PROPERTIES CXX_EXTENSIONS OFF)

# --- The daemon. --------------------------------------------------------------
add_executable(worldd
  main.cpp
)
target_link_libraries(worldd
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd PROPERTIES CXX_EXTENSIONS OFF)

# --- End-to-end integration test (IT-M0 world dispatch path). ------------------
# Self-contained: it generates a throwaway self-signed cert, stands the real TLS
# listener + world serve loop on an ephemeral port, and drives a TLS client that
# sends framed IF-2 messages. No DB or external service, so it always runs in the
# plain `ctest` (unlike the DB-gated authd test).
add_executable(worldd-dispatch-test
  test/world_dispatch_test.cpp
)
target_link_libraries(worldd-dispatch-test
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-dispatch-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-dispatch COMMAND worldd-dispatch-test)
