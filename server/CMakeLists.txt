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
# metrics: OPS-05 in-process Prometheus registry + /metrics HTTP exposer (#164,
# covers #93) — server SAD §8.5, docs/telemetry-architecture.md §5. Clean-room,
# dependency-free (BSD sockets + the exposition-format spec). authd + worldd link
# it to expose the SAD §8.5 metric set the server/ops dashboards query.
add_subdirectory(libs/metrics)
# trace: OPS-05 session-flow tracing (#166) — clean-room span model + a hand-
# rolled OTLP/HTTP-JSON exporter (D-29 §9 rule 5; docs/telemetry-architecture.md
# §5.3). The TRACES leg of the OPS-05 triple beside metrics (#164) + logs (#165).
# Dependency-light (BSD sockets + the OTLP-JSON wire shape; no OTel SDK/protobuf).
# authd + worldd link it to emit the session-flow trace (login -> enter-world).
add_subdirectory(libs/trace)
add_subdirectory(libs/proto)
# bus: the internal message bus (#88) — SAD §2.6/§6.1/§6.3. Typed publish/
# subscribe over bounded, ordered, backpressured lanes: the single mechanism for
# cross-subsystem messaging so subsystems never call each other synchronously
# from the update loop (generalizes worldd's world-thread<->IO-worker MPSC queue,
# #82/#87). Dependency-free (std threads only). Ships a unit test + a "bus-only"
# CI ARCHITECTURE TEST that enforces the discipline — both run in this job's
# ctest. worldd migration onto the bus is a documented follow-up (server/libs/
# bus/test/README.md), scoped out of v0 to avoid destabilising #82/#87.
add_subdirectory(libs/bus)
# srp: SRP-6a authentication (#78) — clean-room from RFC 5054; needs OpenSSL.
add_subdirectory(libs/srp)
# db: MariaDB access (#75) — synchronous connection + prepared statements.
add_subdirectory(libs/db)
# net: TLS 1.3 listener + IF-1 length framing for authd (#77) — clean-room from
# SAD §5.1/§2.1; needs OpenSSL (BSD sockets, no Asio at M0).
add_subdirectory(libs/net)
# characters: the M0 character-stub CRUD (CHR-01 stub, D-11; #85). Composes
# meridian::db over the characters DB `character` table (SAD §4.2/§4.4) — list /
# create / delete with the account_id soft-ref rule + the M0-frozen race/class
# roster. A library the enter-world / character-select flow links; ships a
# DB-backed integration test that self-SKIPs without MERIDIAN_DB_*.
add_subdirectory(characters)
# items: the M1 item system (ITM-01 + ECO-01; #366). Item templates + a read-only
# datastore seam (placeholder set now, mcc #28 later), server-minted item
# instances, the server-authoritative inventory/equip container (add/remove/
# stack/split/merge + equip slot rules), and int64 copper currency ops on the
# character. Composes meridian::db over the characters DB item_instance /
# character_inventory / character.money (SAD §4.2/§4.7). The foundation loot
# (#369) + vendors (#370) build on; ships a DB-free unit test (pure inventory +
# currency logic) and a DB-backed integration test that self-SKIPs without
# MERIDIAN_DB_*.
add_subdirectory(items)
# loot: the M1 loot system (ITM-02; #369). Loot tables (weighted groups + quantity
# ranges + money) with a read-only datastore seam (placeholder set now, mcc #28
# later), the deterministic seeded loot roll, and the corpse loot-session window
# (ownership / in-range / not-already-looted / quest-gated validation + transfer
# into a player inventory via meridian::items #366). worldd's map tick rolls a
# creature's loot table into a session on its corpse at kill time; ships a DB-free
# unit test (seeded determinism, statistical drop rates, session validation).
add_subdirectory(loot)
# vendor: server-authoritative vendor transactions (ECO-01; #370). buy / sell /
# buyback over meridian::items — the vendor catalog seam (placeholder set now, mcc
# #28 later), the per-session buyback queue, and pure + DB-backed transaction logic
# (int64 copper, no client-trusted prices). Ships a DB-free unit test (the economy
# rules) and a DB-backed integration test that self-SKIPs without MERIDIAN_DB_*.
add_subdirectory(vendor)

# --- Daemons (server/README.md process table; SAD §2.1 / §2.5). --------------
add_subdirectory(authd)
add_subdirectory(worldd)
# telemetryd (#167): the OPS-05 client-telemetry INGEST daemon — receives the
# #168 client ERROR/CRITICAL log batches (+ crash / missing-content events),
# validates them against the D-29 privacy contract, rate-limits per build/IP, and
# forwards validated events to the structured-log sink (Loki via the M0 stdout
# JSON sink). Composes meridian::core + meridian::metrics; standalone service so
# the untrusted client-facing ingest fails/scales independently of authd.
add_subdirectory(telemetryd)

# --- Tools. -------------------------------------------------------------------
# meridian-account (#80): the M0 account-creation CLI (ACC-03). Composes
# meridian-srp (make_verifier) + meridian-db (prepared-statement INSERT) to
# register an account into the auth DB `account` table (SAD §2.1, §4.1).
add_subdirectory(tools/meridian-account)

# meridian-character: character-creation CLI for test provisioning / the
# concurrency-harness load driver. Resolves an account_id from the auth DB by
# username, then creates a character through the existing characters CRUD
# (meridian::characters, server/characters/src/characters.h, CHR-01 stub / D-11).
add_subdirectory(tools/meridian-character)

# worldd now consumes world.fbs for real via world-dispatch (#82/#83), which
# links meridian::proto (+ meridian::net) itself (worldd/CMakeLists.txt) — so no
# separate proto link is wired here. authd likewise links meridian::proto via
# authd-login (#79). Both edges keep flatc codegen on the build graph.
