# SPDX-License-Identifier: Apache-2.0
#
# meridian-characters — the M0 character-stub CRUD (CHR-01 stub, D-11; issue #85).
# Composes meridian-db (Connection + prepared statements) over the characters DB
# `character` table (server/db/characters/migrations/0001, server SAD §4.2/§4.4).
# Clean-room, Apache-2.0.

# CRUD as a small library so the enter-world / character-select flow (worldd,
# later a characters service) and the integration test drive the same code path.
add_library(meridian-characters-lib STATIC
  src/characters.cpp
)
add_library(meridian::characters ALIAS meridian-characters-lib)
target_include_directories(meridian-characters-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-characters-lib PUBLIC meridian::db)
target_compile_features(meridian-characters-lib PUBLIC cxx_std_20)
set_target_properties(meridian-characters-lib PROPERTIES CXX_EXTENSIONS OFF)

# Integration test — needs a live MariaDB. Reads MERIDIAN_DB_* env and SKIPS
# (exit 0) when none are set, so it is inert in the plain server build's ctest
# and runs for real only in the DB CI job (or locally, scripts/dev/test.sh --db).
# It drives create/list/delete through the library code path against the
# characters DB `character` table.
add_executable(meridian-characters-test test/characters_test.cpp)
target_link_libraries(meridian-characters-test PRIVATE meridian::characters)
set_target_properties(meridian-characters-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME characters COMMAND meridian-characters-test)
