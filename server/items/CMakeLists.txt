# SPDX-License-Identifier: Apache-2.0
#
# meridian-items — the M1 item system (ITM-01 + ECO-01; issue #366). Item
# templates + the read-only datastore seam, server-minted item instances, the
# server-authoritative inventory/equip container, and int64 copper currency.
# Composes meridian::db over the characters DB `item_instance` /
# `character_inventory` / `character.money` (server SAD §4.2/§4.7,
# server/db/characters/migrations/0001). Clean-room, Apache-2.0.
#
# The foundation library that loot (#369), vendors (#370) and quest rewards link
# to mint/place/destroy items, validate inventory/equip moves, and move copper.

add_library(meridian-items-lib STATIC
  src/item_template.cpp
  src/currency.cpp
  src/inventory.cpp
  src/item_store.cpp
)
add_library(meridian::items ALIAS meridian-items-lib)
target_include_directories(meridian-items-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-items-lib PUBLIC meridian::db)
target_compile_features(meridian-items-lib PUBLIC cxx_std_20)
set_target_properties(meridian-items-lib PROPERTIES CXX_EXTENSIONS OFF)

# Deterministic, DB-FREE unit test: the pure domain logic — item templates /
# placeholder set, the inventory/equip container (add/remove/stack/split/merge,
# equip validation + slot rules) and the pure copper-arithmetic invariants
# (add/subtract, no underflow/overflow). Runs in the plain server ctest (needs no
# MariaDB), so it asserts real behaviour in every CI job and locally.
add_executable(meridian-items-test test/inventory_test.cpp)
target_link_libraries(meridian-items-test PRIVATE meridian::items)
set_target_properties(meridian-items-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME items COMMAND meridian-items-test)

# DB-backed integration test: drives instance minting, placement, inventory load
# round-trip, and the transactional currency ops against a live characters DB.
# Reads MERIDIAN_DB_* and SKIPS (exit 0) when none are set — inert in the DB-free
# ctest, real only in the DB CI job / `scripts/dev/test.sh --db`.
add_executable(meridian-items-db-test test/item_store_it.cpp)
target_link_libraries(meridian-items-db-test PRIVATE meridian::items)
set_target_properties(meridian-items-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME items-db COMMAND meridian-items-db-test)
