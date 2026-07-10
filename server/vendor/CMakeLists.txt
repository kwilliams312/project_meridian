# SPDX-License-Identifier: Apache-2.0
#
# meridian-vendor — server-authoritative vendor transactions (ECO-01; issue #370).
# buy / sell / buyback over the item model: the vendor catalog seam (placeholder
# set now, mcc #28 later), the per-session buyback queue, and the pure + DB-backed
# transaction logic. Composes meridian::items (inventory / instances / int64
# copper currency) over the characters DB. Clean-room, Apache-2.0.
#
# The economy layer the worldd vendor handlers (#370) call to move copper + items;
# loot (#369) and quest rewards reuse the same meridian::items primitives.

add_library(meridian-vendor-lib STATIC
  src/vendor_catalog.cpp
  src/vendor.cpp
)
add_library(meridian::vendor ALIAS meridian-vendor-lib)
target_include_directories(meridian-vendor-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-vendor-lib PUBLIC meridian::items)
target_compile_features(meridian-vendor-lib PUBLIC cxx_std_20)
set_target_properties(meridian-vendor-lib PROPERTIES CXX_EXTENSIONS OFF)

# Deterministic, DB-FREE unit test: the pure economy rules — the buyback queue
# (bounded FIFO, eviction, slot take), the vendor catalog (listing lookup + price
# resolution), and buy/sell/buyback against an in-memory inventory + copper
# balance (debit + grant, credit + remove, insufficient-funds / insufficient-space
# rejection, buyback restore). Runs in the plain server ctest (no MariaDB), so it
# asserts real behaviour in every CI job and locally.
add_executable(meridian-vendor-test test/vendor_test.cpp)
target_link_libraries(meridian-vendor-test PRIVATE meridian::vendor)
set_target_properties(meridian-vendor-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME vendor COMMAND meridian-vendor-test)

# DB-backed integration test: drives buy/sell/buyback end-to-end against a live
# characters DB — copper debited/credited on character.money, item_instance minted/
# destroyed, character_inventory placement created/cleared. Reads MERIDIAN_DB_* and
# SKIPS (exit 0) when none are set — inert in the DB-free ctest, real only in the
# DB CI job / `scripts/dev/test.sh --db`.
add_executable(meridian-vendor-db-test test/vendor_store_it.cpp)
target_link_libraries(meridian-vendor-db-test PRIVATE meridian::vendor)
set_target_properties(meridian-vendor-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME vendor-db COMMAND meridian-vendor-db-test)
