# SPDX-License-Identifier: Apache-2.0
#
# libmeridian-proto — FlatBuffers codegen from /schema/net (server SAD §2 "Shared
# static libraries"; D-01 "schema-generated edges"). Issue #67.
#
# This target runs `flatc --cpp` at BUILD TIME on the two client-facing wire
# contracts the server owns — schema/net/auth.fbs (IF-1) and schema/net/world.fbs
# (IF-2) — producing *_generated.h into the build tree. No generated code is
# committed (schema/net/README.md: "no committed generated code"); the .fbs files
# are the single source of truth.
#
# The library is an INTERFACE target: FlatBuffers C++ output is header-only, and
# so is the flatbuffers runtime it depends on. Consumers link `meridian::proto`
# and #include "auth_generated.h" / "world_generated.h".
#
# ---------------------------------------------------------------------------
# flatc + runtime provisioning — decision
# ---------------------------------------------------------------------------
# We need TWO things from FlatBuffers: the `flatc` compiler (a build tool) and
# the header-only C++ runtime (flatbuffers/flatbuffers.h etc.). We source them
# with a find-first / fetch-fallback strategy so the tree builds BOTH on a dev
# machine (Homebrew: `brew install flatbuffers`) AND on a clean ubuntu CI runner:
#
#   1. find_package(FlatBuffers CONFIG)  — uses an installed FlatBuffers (Homebrew
#      /opt/homebrew, apt `flatbuffers-compiler` + `libflatbuffers-dev`). Fast; no
#      network. Gives us the imported `flatbuffers::flatbuffers` runtime target and
#      the `flatc` executable (imported target `flatbuffers::flatc` when present,
#      else the FLATBUFFERS_FLATC_EXECUTABLE the config sets).
#
#   2. FetchContent fallback (pinned tag) — if no install is found, fetch and build
#      FlatBuffers from source at a PINNED release so a bare runner with only a C++
#      compiler still works. Pinned to v25.12.19 to match the version this was
#      authored against (Homebrew flatc 25.12.19); bump deliberately, never floating.
#
# CI installs the apt packages (see .github/workflows/build.yml, `server` job), so
# path (1) is taken there; path (2) is the safety net for any environment lacking
# a system FlatBuffers.

# --- 1. Locate or fetch FlatBuffers (compiler + runtime). --------------------
set(MERIDIAN_FLATBUFFERS_PINNED_TAG "v25.12.19"
  CACHE STRING "FlatBuffers release tag used by the FetchContent fallback")

find_package(FlatBuffers CONFIG QUIET)

if(FlatBuffers_FOUND)
  message(STATUS "meridian-proto: using system FlatBuffers (find_package)")
else()
  message(STATUS
    "meridian-proto: system FlatBuffers not found — fetching ${MERIDIAN_FLATBUFFERS_PINNED_TAG} via FetchContent")

  include(FetchContent)

  # Build only what we need: the flatc compiler + the runtime. Skip tests/tools.
  set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATC ON  CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATLIB ON CACHE BOOL "" FORCE)
  set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        ${MERIDIAN_FLATBUFFERS_PINNED_TAG}
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(flatbuffers)
endif()

# --- 2. Resolve the flatc executable target/path. ----------------------------
# find_package exposes the imported target `flatbuffers::flatc`; the FetchContent
# build exposes the plain target `flatc`. Normalise to a single command we can
# hand to add_custom_command.
if(TARGET flatbuffers::flatc)
  set(MERIDIAN_FLATC flatbuffers::flatc)
elseif(TARGET flatc)
  set(MERIDIAN_FLATC flatc)
elseif(DEFINED FLATBUFFERS_FLATC_EXECUTABLE)
  set(MERIDIAN_FLATC "${FLATBUFFERS_FLATC_EXECUTABLE}")
else()
  message(FATAL_ERROR
    "meridian-proto: FlatBuffers found but no flatc compiler target/executable resolved")
endif()

# Likewise normalise the runtime target consumers must link against.
if(TARGET flatbuffers::flatbuffers)
  set(MERIDIAN_FLATBUFFERS_RUNTIME flatbuffers::flatbuffers)
elseif(TARGET flatbuffers)
  set(MERIDIAN_FLATBUFFERS_RUNTIME flatbuffers)
else()
  message(FATAL_ERROR
    "meridian-proto: could not resolve the flatbuffers runtime target")
endif()

# --- 3. Codegen: flatc --cpp on the /schema/net contracts. -------------------
# Repo root is two levels up from server/ (server/libs/proto -> server -> root).
set(MERIDIAN_SCHEMA_NET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../schema/net")
cmake_path(NORMAL_PATH MERIDIAN_SCHEMA_NET_DIR)

set(MERIDIAN_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MERIDIAN_PROTO_GEN_DIR}")

# The schemas to compile and the headers each produces (flatc names output
# <stem>_generated.h). Keep the two lists index-aligned.
set(MERIDIAN_PROTO_SCHEMAS
  "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
  "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
)
set(MERIDIAN_PROTO_GENERATED
  "${MERIDIAN_PROTO_GEN_DIR}/auth_generated.h"
  "${MERIDIAN_PROTO_GEN_DIR}/world_generated.h"
)

# One custom command emits both headers in a single flatc invocation. flatc
# resolves the shared `namespace meridian.net` across both files when they are
# compiled together, so pass them as a group.
add_custom_command(
  OUTPUT  ${MERIDIAN_PROTO_GENERATED}
  COMMAND ${MERIDIAN_FLATC}
          --cpp
          --scoped-enums
          -o "${MERIDIAN_PROTO_GEN_DIR}"
          ${MERIDIAN_PROTO_SCHEMAS}
  DEPENDS ${MERIDIAN_PROTO_SCHEMAS} ${MERIDIAN_FLATC}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "meridian-proto: flatc --cpp on schema/net/{auth,world}.fbs"
  VERBATIM
)

# A concrete target that owns the generated outputs, so anything depending on
# meridian-proto triggers codegen before its own compile.
add_custom_target(meridian-proto-codegen
  DEPENDS ${MERIDIAN_PROTO_GENERATED}
)

# --- 4. The consumable library target. ---------------------------------------
# Header-only (generated headers + header-only runtime) => INTERFACE library.
add_library(meridian-proto INTERFACE)
add_library(meridian::proto ALIAS meridian-proto)

add_dependencies(meridian-proto meridian-proto-codegen)

target_include_directories(meridian-proto
  INTERFACE
    "${MERIDIAN_PROTO_GEN_DIR}"
)

target_link_libraries(meridian-proto
  INTERFACE
    ${MERIDIAN_FLATBUFFERS_RUNTIME}
)

target_compile_features(meridian-proto INTERFACE cxx_std_20)

# --- 5. Round-trip proof target (Issue #67 deliverable 2). -------------------
# Builds a SessionGrant into a buffer and reads it back — proves codegen +
# runtime linkage end to end. Runnable as `meridian-proto-roundtrip`.
add_executable(meridian-proto-roundtrip
  test/roundtrip.cpp
)

target_link_libraries(meridian-proto-roundtrip
  PRIVATE
    meridian::proto
)

# --- 6. Golden wire-corpus conformance harness (Issue #68). ------------------
# Protocol drift guard: freezes a canonical instance of every IF-1/IF-2 message
# (schema/net/{auth,world}.fbs) as its FlatBuffer root-table bytes under
# conformance/golden/*.bin, then, on each run, decodes each golden via the real
# meridian::proto codec + asserts the expected logical fields (the version-stable
# wire-drift guard), checks same-build re-encode determinism, and verifies a
# semantic round-trip. A codec/schema change that alters the wire format fails
# this test. These checks are flatc-version-independent — they never compare
# freshly-encoded bytes to the golden bytes (unstable across flatc versions; see
# conformance.cpp header, #264). The client SAD §5.1 "golden-message round-trip"
# conformance requirement for the NETWORK protocol — the network analogue of the
# mcc content-determinism golden (#122).
#
# `--update` regenerates the golden corpus + manifest (a golden diff is a
# wire-protocol diff; review + commit both). The golden dir is passed as an
# absolute path so the harness reads/writes the checked-in corpus from any CWD
# (e.g. ctest runs from the build tree).
add_executable(meridian-proto-conformance
  conformance/conformance.cpp
)

target_link_libraries(meridian-proto-conformance
  PRIVATE
    meridian::proto
)

target_compile_definitions(meridian-proto-conformance
  PRIVATE
    MERIDIAN_PROTO_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/conformance/golden"
)

set_target_properties(meridian-proto-conformance PROPERTIES CXX_EXTENSIONS OFF)

# Register in ctest so the CI `server` job (`ctest --test-dir server/build`)
# runs the drift guard. Default (no args) is --check mode: exit non-zero on any
# decode-mismatch or byte drift vs the golden corpus.
add_test(NAME proto-conformance COMMAND meridian-proto-conformance)
