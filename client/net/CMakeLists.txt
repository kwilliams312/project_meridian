# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — engine-agnostic client NET CORE (issue #95).
#
# A standalone static library (meridian-clientnet) that links WITHOUT godot-cpp, so
# the headless bot, a doctest/ctest suite, and (later) the Godot client can share
# one net stack. It carries the SAME crypto/wire stack the servers use so
# client↔server interoperate byte-for-byte:
#   - plain sockets + OpenSSL TLS 1.3 (auth channel) / plain TCP (world channel)
#   - u32-LE length-prefixed framing (framing.*)
#   - IF-2 world-frame codec (wire_frame.*)
#   - per-session ChaCha20-Poly1305 / HKDF-SHA256 AEAD (world_session.*)
#   - FlatBuffers codec for auth.fbs / world.fbs (codec.*)
#
# Enabled with -DMERIDIAN_CLIENT_NET=ON (default OFF, like MERIDIAN_BOT /
# MERIDIAN_CLIENT_TESTS). Builds in any C++17 toolchain — no Godot runtime. Its
# engine-free unit tests register in ctest (clientnet-test).

cmake_minimum_required(VERSION 3.22)

if(NOT MERIDIAN_CLIENT_NET)
  return()
endif()

find_package(OpenSSL REQUIRED)
find_package(FlatBuffers CONFIG QUIET)

get_filename_component(MERIDIAN_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MERIDIAN_SCHEMA_NET_DIR "${MERIDIAN_REPO_ROOT}/schema/net")

# --- flatc: system or the CONFIG-package imported target --------------------
find_program(MERIDIAN_FLATC_EXE flatc)
if(NOT TARGET flatbuffers::flatc AND MERIDIAN_FLATC_EXE)
  add_executable(flatbuffers::flatc IMPORTED GLOBAL)
  set_target_properties(flatbuffers::flatc PROPERTIES
    IMPORTED_LOCATION "${MERIDIAN_FLATC_EXE}")
endif()
if(TARGET flatbuffers::flatc)
  set(MERIDIAN_CLIENTNET_FLATC flatbuffers::flatc)
elseif(MERIDIAN_FLATC_EXE)
  set(MERIDIAN_CLIENTNET_FLATC "${MERIDIAN_FLATC_EXE}")
else()
  message(FATAL_ERROR
    "meridian-clientnet (#95): no flatc found for auth.fbs/world.fbs codegen. "
    "Install flatbuffers (brew install flatbuffers / apt libflatbuffers-dev).")
endif()

# --- flatc --cpp on schema/net/{auth,world}.fbs -> *_generated.h ------------
set(MERIDIAN_CLIENTNET_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MERIDIAN_CLIENTNET_GEN_DIR}")
set(MERIDIAN_CLIENTNET_GENERATED
  "${MERIDIAN_CLIENTNET_GEN_DIR}/auth_generated.h"
  "${MERIDIAN_CLIENTNET_GEN_DIR}/world_generated.h")
add_custom_command(
  OUTPUT ${MERIDIAN_CLIENTNET_GENERATED}
  COMMAND ${MERIDIAN_CLIENTNET_FLATC} --cpp --scoped-enums
          -o "${MERIDIAN_CLIENTNET_GEN_DIR}"
          "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
          "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
  DEPENDS "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
          "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
  COMMENT "meridian-clientnet: flatc --cpp on schema/net/{auth,world}.fbs"
  VERBATIM)
add_custom_target(meridian-clientnet-codegen DEPENDS ${MERIDIAN_CLIENTNET_GENERATED})

# --- The engine-agnostic net core static library ---------------------------
add_library(meridian-clientnet STATIC
  src/framing.cpp
  src/wire_frame.cpp
  src/world_session.cpp
  src/tcp_transport.cpp
  src/tls_client.cpp
  src/codec.cpp
)
add_dependencies(meridian-clientnet meridian-clientnet-codegen)
target_include_directories(meridian-clientnet PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The generated headers are an implementation detail (only codec.cpp includes them),
# so they are PRIVATE — a consumer of the library never sees flatc output.
target_include_directories(meridian-clientnet PRIVATE
  "${MERIDIAN_CLIENTNET_GEN_DIR}")
if(TARGET flatbuffers::flatbuffers)
  target_link_libraries(meridian-clientnet PUBLIC flatbuffers::flatbuffers)
endif()
target_link_libraries(meridian-clientnet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(meridian-clientnet PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(meridian-clientnet PRIVATE /W4)
else()
  target_compile_options(meridian-clientnet PRIVATE -Wall -Wextra)
endif()

# --- Engine-free unit tests (ctest) ----------------------------------------
enable_testing()

add_executable(clientnet-test test/clientnet_test.cpp)
add_dependencies(clientnet-test meridian-clientnet-codegen)
target_link_libraries(clientnet-test PRIVATE meridian-clientnet OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(clientnet-test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(clientnet-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME clientnet COMMAND clientnet-test)
