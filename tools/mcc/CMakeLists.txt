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
    src/stages/emit_sql.cpp
    src/stages/emit_pck.cpp
    src/hash/blake3.cpp)

# The single-file `--watch` loop (check.cpp) uses std::thread; some libstdc++
# toolchains need an explicit pthread link. Threads::Threads is a no-op where
# the standard library already pulls it in (e.g. macOS libc++).
find_package(Threads REQUIRED)

target_include_directories(mcc_stages PUBLIC src)
target_link_libraries(mcc_stages PUBLIC yaml-cpp::yaml-cpp Threads::Threads)

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
