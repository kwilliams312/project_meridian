# SPDX-License-Identifier: Apache-2.0
#
# meridian-character — character-creation CLI for test provisioning / the
# concurrency-harness load driver (mirrors meridian-account, #80). Resolves an
# account_id from the auth DB by username, then creates a character through
# the existing characters CRUD (meridian::characters,
# server/characters/src/characters.h). Clean-room, Apache-2.0.

# Character-creation code path as a small library, so the test can drive the
# exact path the CLI uses.
add_library(meridian-character-lib STATIC
  src/character.cpp
)
add_library(meridian::character ALIAS meridian-character-lib)
target_include_directories(meridian-character-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-character-lib PUBLIC meridian::characters meridian::db)
target_compile_features(meridian-character-lib PUBLIC cxx_std_20)
set_target_properties(meridian-character-lib PROPERTIES CXX_EXTENSIONS OFF)

# The CLI binary.
add_executable(meridian-character src/main.cpp)
target_link_libraries(meridian-character PRIVATE meridian::character)
set_target_properties(meridian-character PROPERTIES CXX_EXTENSIONS OFF)

# Test binary. The arg-parsing/defaulting unit tests are DB-free and always
# run. The resolve-account-id + create-character section additionally needs a
# live MariaDB with both the meridian_auth and meridian_characters schemas
# loaded; it reads MERIDIAN_DB_* env and SKIPS (exit 0) when none are set —
# the same inert-by-default pattern as meridian-account-test /
# meridian-characters-test, so it is inert in the plain server build's ctest
# and runs for real only with env set (or a dedicated CI job).
add_executable(meridian-character-test test/character_test.cpp)
target_link_libraries(meridian-character-test PRIVATE meridian::character)
set_target_properties(meridian-character-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME character COMMAND meridian-character-test)
