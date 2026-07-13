# tools/mcc/CMakeLists.txt — Meridian Content Compiler (TLS-01).
#
# This build produces the `mcc` CLI plus the first real pipeline stages:
# discover/parse (yaml-cpp) and the structural lint engine (L001-L011). The only
# external dependency so far is yaml-cpp (Tools SAD §2). FlatBuffers, vendored
# Recast/Detour, BLAKE3, libvorbis/libogg and the MariaDB client arrive with the
# link/bake/emit stages in later M0 tasks.
#
# Contract: docs/sad/tools-sad.md §2 · docs/prd/tools-prd.md §2.2

cmake_minimum_required(VERSION 3.24)

project(mcc
    VERSION 0.0.0
    DESCRIPTION "Meridian Content Compiler"
    LANGUAGES CXX)

# ---- Language standard (Tools SAD §2: C++20, static CLI) --------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release-ish build when the generator is single-config and the
# caller did not pick one, so a bare `cmake --preset default` is optimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- yaml-cpp (Tools SAD §2 parse stage) -----------------------------------
# Prefer the system package (Homebrew / distro / vcpkg all ship a yaml-cpp CMake
# config). Fall back to a pinned FetchContent build so CI runners without the
# system library still build hermetically. Either path exposes the same
# `yaml-cpp::yaml-cpp` target.
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
    message(STATUS "mcc: system yaml-cpp not found — building it via FetchContent")
    include(FetchContent)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    # yaml-cpp 0.8.0 ships `cmake_minimum_required(VERSION 3.4)`, which CMake 4.x
    # refuses. Grant it a modern policy baseline so the fallback builds on newer
    # CMake (harmless on older CMake, which ignores the unknown-but-valid value).
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG 0.8.0)  # pinned for determinism
    FetchContent_MakeAvailable(yaml-cpp)
else()
    message(STATUS "mcc: using system yaml-cpp ${yaml-cpp_VERSION}")
endif()

# ---- FlatBuffers (Tools SAD §3 chunk-emit — IF-6 ServerChunk payload) -------
# The chunk-emit stage bakes the real schema/chunk/chunk.fbs `ServerChunk`
# payload, so mcc needs the `flatc` compiler (a build tool) + the header-only C++
# runtime. Same find-first / fetch-fallback strategy as server/libs/proto so the
# tree builds on a dev machine (Homebrew `flatbuffers`) AND a clean CI runner.
# Pinned to the same tag as the server proto lib so flatc output is consistent.
set(MCC_FLATBUFFERS_PINNED_TAG "v25.12.19"
    CACHE STRING "FlatBuffers release tag used by the FetchContent fallback")

find_package(FlatBuffers CONFIG QUIET)
if(FlatBuffers_FOUND)
    message(STATUS "mcc: using system FlatBuffers (find_package)")
else()
    message(STATUS
        "mcc: system FlatBuffers not found — fetching ${MCC_FLATBUFFERS_PINNED_TAG} via FetchContent")
    include(FetchContent)
    set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FLATBUFFERS_BUILD_FLATC ON  CACHE BOOL "" FORCE)
    set(FLATBUFFERS_BUILD_FLATLIB ON CACHE BOOL "" FORCE)
    set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(flatbuffers
        GIT_REPOSITORY https://github.com/google/flatbuffers.git
        GIT_TAG        ${MCC_FLATBUFFERS_PINNED_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(flatbuffers)
endif()

# Normalise the flatc executable target and the runtime target (the two provision
# paths name them differently — imported `flatbuffers::flatc` vs plain `flatc`).
if(TARGET flatbuffers::flatc)
    set(MCC_FLATC flatbuffers::flatc)
elseif(TARGET flatc)
    set(MCC_FLATC flatc)
elseif(DEFINED FLATBUFFERS_FLATC_EXECUTABLE)
    set(MCC_FLATC "${FLATBUFFERS_FLATC_EXECUTABLE}")
else()
    message(FATAL_ERROR "mcc: FlatBuffers found but no flatc compiler resolved")
endif()

if(TARGET flatbuffers::flatbuffers)
    set(MCC_FLATBUFFERS_RUNTIME flatbuffers::flatbuffers)
elseif(TARGET flatbuffers)
    set(MCC_FLATBUFFERS_RUNTIME flatbuffers)
else()
    message(FATAL_ERROR "mcc: could not resolve the flatbuffers runtime target")
endif()

# flatc --cpp on schema/chunk/chunk.fbs -> chunk_generated.h in the build tree
# (no committed generated code — the .fbs is the single source of truth, same
# discipline as schema/net). Repo root is two levels up (tools/mcc -> tools -> root).
set(MCC_CHUNK_FBS "${CMAKE_CURRENT_SOURCE_DIR}/../../schema/chunk/chunk.fbs")
cmake_path(NORMAL_PATH MCC_CHUNK_FBS)
set(MCC_CHUNK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MCC_CHUNK_GEN_DIR}")
set(MCC_CHUNK_GENERATED "${MCC_CHUNK_GEN_DIR}/chunk_generated.h")

add_custom_command(
    OUTPUT  ${MCC_CHUNK_GENERATED}
    COMMAND ${MCC_FLATC} --cpp --scoped-enums -o "${MCC_CHUNK_GEN_DIR}" ${MCC_CHUNK_FBS}
    DEPENDS ${MCC_CHUNK_FBS} ${MCC_FLATC}
    COMMENT "mcc: flatc --cpp on schema/chunk/chunk.fbs"
    VERBATIM)
add_custom_target(mcc-chunk-codegen DEPENDS ${MCC_CHUNK_GENERATED})

# ---- Pipeline stages as a small static lib ---------------------------------
# The stage sources are shared between the `mcc` CLI and the unit-test binary,
# so they live in an OBJECT/STATIC library rather than being compiled twice.
add_library(mcc_stages STATIC
    src/stages/diagnostics.cpp
    src/stages/discover.cpp
    src/stages/parse.cpp
    src/stages/validate.cpp
    src/stages/check.cpp
    src/stages/format.cpp
    src/stages/idmap.cpp
    src/stages/link.cpp
    src/stages/index.cpp
    src/stages/content_hash.cpp
    src/stages/pack_contract.cpp
    src/stages/emit_sql.cpp
    src/stages/emit_pck.cpp
    src/stages/chunk_emit.cpp
    src/hash/blake3.cpp)

# chunk_emit.cpp #includes the flatc-generated chunk_generated.h — make the stage
# lib depend on the codegen and see the generated dir + the flatbuffers runtime.
add_dependencies(mcc_stages mcc-chunk-codegen)
target_include_directories(mcc_stages PUBLIC "${MCC_CHUNK_GEN_DIR}")
target_link_libraries(mcc_stages PUBLIC ${MCC_FLATBUFFERS_RUNTIME})

# The single-file `--watch` loop (check.cpp) uses std::thread; some libstdc++
# toolchains need an explicit pthread link. Threads::Threads is a no-op where
# the standard library already pulls it in (e.g. macOS libc++).
find_package(Threads REQUIRED)

target_include_directories(mcc_stages PUBLIC src)
target_link_libraries(mcc_stages PUBLIC yaml-cpp::yaml-cpp Threads::Threads)

# The stage objects are also linked into libmccore (a SHARED library, below), so
# they must be position-independent. Harmless for the static CLI/test links.
set_target_properties(mcc_stages PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- The single `mcc` executable -------------------------------------------
add_executable(mcc src/main.cpp)

target_link_libraries(mcc PRIVATE mcc_stages)

# Version string surfaced by `mcc --version`, sourced from the project version
# so there is one place to bump it.
target_compile_definitions(mcc PRIVATE
    MCC_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(mcc_stages PRIVATE /W4 /permissive-)
    target_compile_options(mcc PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc_stages PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mcc PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mcc RUNTIME DESTINATION bin)

# ---- libmccore: the C ABI shared library (TLS-03/04, #125) -----------------
# A stable C-linkage (`extern "C"`) surface over the same pipeline stages the
# CLI uses, so the Codex editor (.NET/Avalonia, #124) can drive
# validate/index/pickable/backlinks IN-PROCESS via P/Invoke instead of shelling
# out to `mcc` (SAD §2 "libmccore", §6). It links the SAME `mcc_stages` objects
# — no reimplementation, so editor and CI can never disagree (SAD §6.4). The mcc
# CLI is unaffected: this is an additional target that shares the stage code.
add_library(mccore SHARED
    libmccore/mccore.cpp)

target_link_libraries(mccore PRIVATE mcc_stages)
# The public header lives beside the source; consumers include just "mccore.h".
target_include_directories(mccore PUBLIC libmccore)

# Stamp the CLI's version into the library's build string (mccore_version_string).
target_compile_definitions(mccore PRIVATE
    MCC_VERSION="${PROJECT_VERSION}")

# Hide everything except the MCCORE_API-annotated `extern "C"` entry points, so
# the ABI surface is exactly the documented functions (and the C++ stage symbols
# do not leak / clash). MCCORE_BUILDING_DLL selects dllexport on Windows.
set_target_properties(mccore PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    C_VISIBILITY_PRESET hidden)
target_compile_definitions(mccore PRIVATE MCCORE_BUILDING_DLL)

if(MSVC)
    target_compile_options(mccore PRIVATE /W4 /permissive-)
else()
    target_compile_options(mccore PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mccore LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES libmccore/mccore.h DESTINATION include)

# ---- Unit tests (mcc-tests) ------------------------------------------------
# A dependency-light, self-contained test binary (no GoogleTest — keeps the
# hermetic build's dependency surface at just yaml-cpp). Registered with CTest
# so `ctest --test-dir build` runs it; CI can invoke it the same way it runs the
# server's ctest suite. Proves the fmt guarantees: idempotency, semantic
# preservation, --check drift detection, and ugly->canonical golden fixtures.
enable_testing()

add_executable(mcc-tests
    tests/test_format.cpp)
target_link_libraries(mcc-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-fmt-unit COMMAND mcc-tests)

# Single-file / validation-as-you-type tests (`mcc check --file`, SAD §6.3):
# valid/invalid verdicts, graceful cross-file-ref degradation, stable JSON
# envelope, and the --watch re-validation loop.
add_executable(mcc-check-file-tests
    tests/test_check_file.cpp)
target_link_libraries(mcc-check-file-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-check-file-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-check-file-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-check-file-unit COMMAND mcc-check-file-tests)

# Link stage + IF-9 idmap allocator tests (SAD §2.3/§2.4): reference resolution
# + dangling-ref diagnostic, backlink index, idmap determinism + stability, and
# idmap.lock round-trip.
add_executable(mcc-link-tests
    tests/test_link.cpp)
target_link_libraries(mcc-link-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-link-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-link-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-link-unit COMMAND mcc-link-tests)

# ID index + typed reference pickers + backlinks tests (SAD §2.3/§6, #127):
# id-by-type grouping + id->numeric/type/file resolution, typed pickers returning
# only valid-type candidates (schema `*Ref` grounding), find-usages backlinks
# (with field path; empty for orphans, unresolved for unknown ids), and stable +
# parseable JSON contracts (id-index/pickable/backlinks).
add_executable(mcc-index-tests
    tests/test_index.cpp)
target_link_libraries(mcc-index-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-index-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-index-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-index-unit COMMAND mcc-index-tests)

# pack-contract tests (SAD §2.4, pack-contract spec §3, story #654): the idmap
# append-only lint (L016 — a clean lock passes; a renumber collision, a retired-
# index reuse, a dual map/retired listing, and a reserved index 0 each fail), the
# `mcc diff` breaking-change classifier (additive edit -> additive + exit 0; a
# removed/renumbered id or removed field -> breaking + exit 1 + the exact id/field
# named; a breaking diff with no compatibility_version bump flagged), and the
# pack content_hash (stable across runs; changes iff content changes).
add_executable(mcc-pack-contract-tests
    tests/test_pack_contract.cpp)
target_link_libraries(mcc-pack-contract-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-pack-contract-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-pack-contract-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-pack-contract-unit COMMAND mcc-pack-contract-tests)

# emit-sql stage + vendored BLAKE3 tests (SAD §2.6, IF-4): BLAKE3 correctness vs
# published vectors, well-formed world DB SQL + a world_manifest row (64-hex
# BLAKE3 hash, schema_version=1 — worldd #89's read shape), determinism (same
# content -> identical SQL), and *Ref -> IF-9 numeric-id resolution.
add_executable(mcc-emit-sql-tests
    tests/test_emit_sql.cpp)
target_link_libraries(mcc-emit-sql-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-emit-sql-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-emit-sql-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-emit-sql-unit COMMAND mcc-emit-sql-tests)

# emit-pck stage tests (SAD §2.7, IF-5): well-formed pack.manifest.json (namespace,
# version, pinned godot, id_band, 64-hex BLAKE3 content_hash, entry list with
# IF-9 numeric ids + res:// paths), the IF-4/IF-5 content-hash agreement invariant
# (emit-pck hash == emit-sql world_manifest hash — the three-way tie), determinism
# (same content -> identical manifest), and entry-list coverage (entry count ==
# allocated idmap id count).
add_executable(mcc-emit-pck-tests
    tests/test_emit_pck.cpp)
target_link_libraries(mcc-emit-pck-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-emit-pck-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-emit-pck-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-emit-pck-unit COMMAND mcc-emit-pck-tests)

# chunk-emit stage tests (SAD §3, IF-6, #553): the procedural Zone-01 fixture
# round-trip — the <zone>.chunks.json manifest carries every schema-required field
# (non-zero origin, negative-index grid bounds, chunk_size_m, far_ring, per-chunk
# C2 refs + aabb + priority + deps), each <cx>_<cz>.chunk.bin parses via the
# FlatBuffers verifier as a ServerChunk with a NON-FLAT 129×129 heightfield, the
# recomputed per-chunk BLAKE3 (over both payloads) matches the manifest, the IF-5
# pack contains every referenced asset incl. the server chunk, and the whole emit
# is deterministic (byte-identical across runs).
add_executable(mcc-chunk-emit-tests
    tests/test_chunk_emit.cpp)
target_link_libraries(mcc-chunk-emit-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-chunk-emit-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-chunk-emit-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
# The test reads the checked-in manifest schema to assert every required property
# is emitted; pass its path so the test needs no argv/cwd.
target_compile_definitions(mcc-chunk-emit-tests PRIVATE
    MCC_CHUNK_MANIFEST_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../../schema/chunk/chunk-manifest.schema.yaml")

add_test(NAME mcc-chunk-emit-unit COMMAND mcc-chunk-emit-tests)

# dye/appearance pipeline test (contract ① C1): the CLIENT-side content types
# classify + validate their envelope (incl. the irregular appearance_catalog
# name), flow into the client pck, and produce NO world.sql (spec §8).
add_executable(mcc-dye-pipeline-tests
    tests/test_dye_pipeline.cpp)
target_link_libraries(mcc-dye-pipeline-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-dye-pipeline-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-dye-pipeline-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mcc-dye-pipeline-unit COMMAND mcc-dye-pipeline-tests)

# DB-backed emit-sql integration test (SAD §2.6, IF-4): loads the world DDL + the
# mcc-emitted SQL into a REAL MariaDB and asserts world_manifest + content tables
# are populated. ENV-GUARDED — SKIPS (exit 0) without MERIDIAN_DB_*/MERIDIAN_WORLDDB_*
# (parity with the worldd DB tests). Shells out to the `mariadb`/`mysql` client so
# mcc keeps its dependency surface at just yaml-cpp (no MariaDB client lib link).
add_executable(mcc-emit-sql-db-tests
    tests/test_emit_sql_db.cpp)
target_link_libraries(mcc-emit-sql-db-tests PRIVATE mcc_stages)
if(MSVC)
    target_compile_options(mcc-emit-sql-db-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc-emit-sql-db-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Pass the repo-root world DDL dir + a sample content dir to the DB test so it can
# load the real schema and emit real content. Resolved at configure time.
target_compile_definitions(mcc-emit-sql-db-tests PRIVATE
    MCC_WORLD_DDL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../schema/sql/world"
    MCC_SAMPLE_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../content")

add_test(NAME mcc-emit-sql-db COMMAND mcc-emit-sql-db-tests)

# ---- libmccore C ABI smoke test (#125) -------------------------------------
# A PURE C (C11) translation unit linked against the libmccore shared library.
# Being C-only, it proves the ABI is callable across the C boundary (no C++
# headers, no name mangling): it would fail to link if the surface were not
# genuinely `extern "C"`. It opens the real `content/` root and asserts the
# validate/index/pickable/backlinks/resolve answers match `mcc check/index/
# pickable/refs`, plus alloc/free discipline. Registered with CTest so the #232
# mcc CI job (which runs ctest) exercises it — no workflow edits needed.
enable_language(C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(mccore-smoke
    libmccore/tests/smoke.c)
target_link_libraries(mccore-smoke PRIVATE mccore)

# The content ROOT is the repo's top-level content/ dir (which contains the
# `core` pack). Resolved at configure time so the test needs no argv/cwd.
target_compile_definitions(mccore-smoke PRIVATE
    MCCORE_TEST_CONTENT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../content")

if(MSVC)
    target_compile_options(mccore-smoke PRIVATE /W4)
else()
    target_compile_options(mccore-smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME mccore-abi-smoke COMMAND mccore-smoke)
