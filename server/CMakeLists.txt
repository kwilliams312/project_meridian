# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — server build root.
#
# The authoritative Linux C++20 server container group (server/README.md,
# server SAD §2). M0 build skeleton: one shared static library
# (libmeridian-core) linked into two stub daemons (authd, worldd).
#
# Dependencies arrive with the real subsystems they belong to (SAD §7 build
# plan): FlatBuffers with libmeridian-proto (#67), OpenSSL with libmeridian-srp
# (#78). libmeridian-core stays dependency-free.
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

# Enable CTest at the root so `ctest --test-dir server/build` aggregates tests
# registered in subdirectories (e.g. libs/srp's RFC 5054 vector test, #78).
enable_testing()

# --- Shared libraries (SAD §2). -----------------------------------------------
# core: logging/config/version. proto: FlatBuffers codegen from /schema/net
# (libmeridian-proto, #67 — flatc runs at build time; needs flatc + the runtime,
# see server/libs/proto/CMakeLists.txt for the find-first / fetch-fallback).
add_subdirectory(libs/core)
add_subdirectory(libs/proto)
# srp: SRP-6a authentication (#78) — clean-room from RFC 5054; needs OpenSSL.
add_subdirectory(libs/srp)
# db: MariaDB access (#75) — synchronous connection + prepared statements.
add_subdirectory(libs/db)
# net: TLS 1.3 listener + IF-1 length framing for authd (#77) — clean-room from
# SAD §5.1/§2.1; needs OpenSSL (BSD sockets, no Asio at M0).
add_subdirectory(libs/net)

# --- Daemons (server/README.md process table; SAD §2.1 / §2.5). --------------
add_subdirectory(authd)
add_subdirectory(worldd)

# --- Tools. -------------------------------------------------------------------
# meridian-account (#80): the M0 account-creation CLI (ACC-03). Composes
# meridian-srp (make_verifier) + meridian-db (prepared-statement INSERT) to
# register an account into the auth DB `account` table (SAD §2.1, §4.1).
add_subdirectory(tools/meridian-account)

# Link libmeridian-proto into worldd (#67, deliverable 3) — codegen + linkage
# proof: worldd does not USE the generated IF-2 tables yet (that arrives with the
# real world path, SAD §7), but wiring the link keeps flatc codegen on the build
# graph and proves the schema-generated edge compiles into the daemon.
# authd now consumes auth.fbs for real via authd-login (#79), which links
# meridian::proto itself — so no separate proto link is wired here for authd.
target_link_libraries(worldd PRIVATE meridian::proto)
