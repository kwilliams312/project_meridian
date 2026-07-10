# SPDX-License-Identifier: Apache-2.0
#
# meridian-loot — the M1 loot system (ITM-02; issue #369). Loot tables (weighted
# groups + quantity ranges + money) with a read-only datastore seam (placeholder
# set now, mcc #28 later), the deterministic seeded loot roll, and the corpse
# loot-session window (ownership / in-range / not-already-looted / quest-gated
# validation + transfer into a player inventory). Builds ON meridian::items (the
# ITM-01 item model + inventory API #366) — a loot pull mints-then-places through
# that single server-authoritative inventory path. Clean-room, Apache-2.0.
#
# Consumed by worldd (map_tick): on creature death the map rolls the creature's
# loot table into a session on the corpse, and clients loot items into inventory.

add_library(meridian-loot-lib STATIC
  src/loot_table.cpp
  src/loot_roll.cpp
  src/loot_session.cpp
)
add_library(meridian::loot ALIAS meridian-loot-lib)
target_include_directories(meridian-loot-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-loot-lib PUBLIC meridian::items)
target_compile_features(meridian-loot-lib PUBLIC cxx_std_20)
set_target_properties(meridian-loot-lib PROPERTIES CXX_EXTENSIONS OFF)

# Deterministic, DB-FREE unit test: the pure loot domain — seeded roll determinism
# (a fixed seed → a fixed roll), statistical drop rates over 10^5 draws (server PRD
# §7), the placeholder table set's shape, and the loot session's four validation
# gates (ownership, in-range, no-double-loot, quest-gating) + the transfer of a
# looted stack into a player inventory (ownership moves corpse → player). Runs in
# the plain server ctest (needs no MariaDB), so it asserts real behaviour in every
# CI job and locally.
add_executable(meridian-loot-test test/loot_test.cpp)
target_link_libraries(meridian-loot-test PRIVATE meridian::loot)
set_target_properties(meridian-loot-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME loot COMMAND meridian-loot-test)
