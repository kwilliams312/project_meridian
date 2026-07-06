# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — server build root.
#
# The authoritative Linux C++20 server container group (server/README.md,
# server SAD §2). M0 build skeleton: one shared static library
# (libmeridian-core) linked into two stub daemons (authd, worldd).
#
# ZERO external dependencies at M0 — no OpenSSL/Asio/MariaDB/FlatBuffers. Those
# arrive with the real subsystems they belong to (SAD §7 build plan). This keeps
# a fresh checkout buildable with only a C++20 compiler + CMake + a generator.
#
# Clean-room (CONTRIBUTING.md): architecturally inspired by CMaNGOS/TrinityCore,
# but implemented from the written specs in /docs. No GPL source consulted.

cmake_minimum_required(VERSION 3.20)

project(meridian_server
  VERSION 0.0.0
  DESCRIPTION "Project Meridian game server (clean-room, Apache-2.0)"
  LANGUAGES CXX
)

# --- Standard: C++20, no compiler extensions, required. ----------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a reasonable build type for single-config generators so a bare
# `cmake --preset default` produces optimised-but-debuggable binaries.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Export compile_commands.json for clangd / tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings-forward hygiene for hand-written code. Not -Werror at M0 skeleton
# stage; CI can tighten this once the tree stabilises.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Shared libraries (SAD §2). Only core exists at M0. ----------------------
add_subdirectory(libs/core)

# --- Daemons (server/README.md process table; SAD §2.1 / §2.5). --------------
add_subdirectory(authd)
add_subdirectory(worldd)
