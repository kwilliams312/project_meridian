# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — HEADLESS client-world probe (#301). meridian-client-probe drives
# the GUI client's WORLD-SESSION net path (net::NetThreadCore + the shared
# meridian-clientnet stack — the SAME code scenes/world/world.gd runs) against a real
# authd + worldd with NO Godot and NO display, and reports the #87 AoI relay frames
# (EntityEnter/EntityUpdate) it received about a bot. This is the client-side mirror
# of meridian-two-bot: the see-a-bot-move proof for the GUI net path.
#
# Enabled with -DMERIDIAN_CLIENT_PROBE=ON (default OFF). It links the #97 net-thread
# core + the #99 login core + the #102 movement core + the #104 remote interpolator
# from client/gdextension/meridian/src (compiled here WITHOUT godot-cpp — they are
# engine-free, Client SAD §9.2) on top of meridian-clientnet. Builds in any C++17
# toolchain — no Godot runtime. The live harness is client/test/run_client_sees_bot_it.sh.

cmake_minimum_required(VERSION 3.22)

if(NOT MERIDIAN_CLIENT_PROBE)
  return()
endif()

# The probe rides the engine-agnostic net core; ensure it is present.
if(NOT TARGET meridian-clientnet)
  message(FATAL_ERROR
    "meridian-client-probe (#301) needs meridian-clientnet — configure with "
    "-DMERIDIAN_CLIENT_NET=ON as well as -DMERIDIAN_CLIENT_PROBE=ON.")
endif()

find_package(OpenSSL REQUIRED)
find_package(FlatBuffers CONFIG QUIET)
find_package(Threads REQUIRED)

get_filename_component(MERIDIAN_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MERIDIAN_CLIENT_SRC "${MERIDIAN_REPO_ROOT}/client/gdextension/meridian/src")
set(MERIDIAN_SCHEMA_NET_DIR "${MERIDIAN_REPO_ROOT}/schema/net")

# --- flatc: system or the CONFIG-package imported target -------------------
find_program(MERIDIAN_FLATC_EXE flatc)
if(NOT TARGET flatbuffers::flatc AND MERIDIAN_FLATC_EXE)
  add_executable(flatbuffers::flatc IMPORTED GLOBAL)
  set_target_properties(flatbuffers::flatc PROPERTIES
    IMPORTED_LOCATION "${MERIDIAN_FLATC_EXE}")
endif()
if(TARGET flatbuffers::flatc)
  set(MERIDIAN_PROBE_FLATC flatbuffers::flatc)
elseif(MERIDIAN_FLATC_EXE)
  set(MERIDIAN_PROBE_FLATC "${MERIDIAN_FLATC_EXE}")
else()
  message(FATAL_ERROR
    "meridian-client-probe (#301): no flatc found for auth.fbs/world.fbs codegen. "
    "Install flatbuffers (brew install flatbuffers / apt libflatbuffers-dev).")
endif()

# --- flatc --cpp on schema/net/{auth,world}.fbs -> *_generated.h (login_core dep) --
set(MERIDIAN_PROBE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MERIDIAN_PROBE_GEN_DIR}")
set(MERIDIAN_PROBE_GENERATED
  "${MERIDIAN_PROBE_GEN_DIR}/auth_generated.h"
  "${MERIDIAN_PROBE_GEN_DIR}/world_generated.h")
add_custom_command(
  OUTPUT ${MERIDIAN_PROBE_GENERATED}
  COMMAND ${MERIDIAN_PROBE_FLATC} --cpp --scoped-enums -o "${MERIDIAN_PROBE_GEN_DIR}"
          "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
          "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
  DEPENDS "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs" "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
  COMMENT "meridian-client-probe: flatc --cpp on schema/net/{auth,world}.fbs"
  VERBATIM)
add_custom_target(meridian-client-probe-codegen DEPENDS ${MERIDIAN_PROBE_GENERATED})

# --- The engine-free probe core (static lib shared by the CLI + the test) ----
add_library(meridian-client-probe-core STATIC
  client_world_probe_core.cpp
  # The GUI net path + its cores, compiled engine-free (no godot-cpp):
  "${MERIDIAN_CLIENT_SRC}/net_thread_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/login_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/srp_client_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/login_transport.cpp"
  "${MERIDIAN_CLIENT_SRC}/version_compat.cpp"
  "${MERIDIAN_CLIENT_SRC}/movement_controller.cpp"
  "${MERIDIAN_CLIENT_SRC}/remote_interpolation.cpp"
)
add_dependencies(meridian-client-probe-core meridian-client-probe-codegen)
target_include_directories(meridian-client-probe-core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${MERIDIAN_CLIENT_SRC}")
target_include_directories(meridian-client-probe-core PRIVATE
  "${MERIDIAN_PROBE_GEN_DIR}")
target_link_libraries(meridian-client-probe-core PUBLIC
  meridian-clientnet Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(TARGET flatbuffers::flatbuffers)
  target_link_libraries(meridian-client-probe-core PUBLIC flatbuffers::flatbuffers)
endif()
set_target_properties(meridian-client-probe-core PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(meridian-client-probe-core PRIVATE -Wall -Wextra)
endif()

# --- The CLI (the live see-a-bot-move driver) -------------------------------
add_executable(meridian-client-probe client_world_probe_main.cpp)
target_link_libraries(meridian-client-probe PRIVATE meridian-client-probe-core)
set_target_properties(meridian-client-probe PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(meridian-client-probe PRIVATE -Wall -Wextra)
endif()

# --- Engine-free handoff unit test (ctest) ----------------------------------
enable_testing()
add_executable(client-world-probe-test test/client_world_probe_test.cpp)
target_link_libraries(client-world-probe-test PRIVATE meridian-client-probe-core)
set_target_properties(client-world-probe-test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(client-world-probe-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME client-world-probe COMMAND client-world-probe-test)
