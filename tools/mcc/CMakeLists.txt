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
    src/stages/format.cpp)

target_include_directories(mcc_stages PUBLIC src)
target_link_libraries(mcc_stages PUBLIC yaml-cpp::yaml-cpp)

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
