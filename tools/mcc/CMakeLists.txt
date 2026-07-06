# tools/mcc/CMakeLists.txt — Meridian Content Compiler (TLS-01), v0 skeleton.
#
# Self-contained, ZERO external dependencies. yaml-cpp, FlatBuffers, vendored
# Recast/Detour, BLAKE3, libvorbis/libogg and the MariaDB client all arrive with
# the real stages in later M0 tasks (Tools SAD §2). This build produces only the
# `mcc` CLI shell: a subcommand dispatcher over stubbed pipeline stages.
#
# Contract: docs/sad/tools-sad.md §2 · docs/prd/tools-prd.md §2.2

cmake_minimum_required(VERSION 3.24)

project(mcc
    VERSION 0.0.0
    DESCRIPTION "Meridian Content Compiler — CLI skeleton"
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

# ---- The single `mcc` executable -------------------------------------------
add_executable(mcc
    src/main.cpp)

target_include_directories(mcc PRIVATE src)

# Version string surfaced by `mcc --version`, sourced from the project version
# so there is one place to bump it.
target_compile_definitions(mcc PRIVATE
    MCC_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(mcc PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcc PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mcc RUNTIME DESTINATION bin)
