# SPDX-License-Identifier: Apache-2.0
#
# meridian-db — MariaDB access (server SAD §2.2, TD-05). M0: synchronous
# connection + prepared statements over MariaDB Connector/C (libmariadb).

find_package(PkgConfig REQUIRED)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb)
find_package(Threads REQUIRED)  # concurrency regression test (#510)

add_library(meridian-db STATIC
  src/connection.cpp
)
add_library(meridian::db ALIAS meridian-db)

target_include_directories(meridian-db
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(meridian-db PUBLIC PkgConfig::MARIADB)
target_compile_features(meridian-db PUBLIC cxx_std_20)
set_target_properties(meridian-db PROPERTIES CXX_EXTENSIONS OFF)

# Integration test — needs a live MariaDB. Reads connection params from
# MERIDIAN_DB_* env vars and SKIPS (exit 0) when none are set, so it is inert in
# the plain server build's ctest and runs for real only in the DB CI job (or
# locally with env set).
add_executable(meridian-db-test test/db_test.cpp)
target_link_libraries(meridian-db-test PRIVATE meridian::db)
set_target_properties(meridian-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME db COMMAND meridian-db-test)

# Concurrency regression test (#510): N threads x M iterations, each opening its
# own connection and asserting a parameterized SELECT returns the seeded row.
# Reproduces the libmariadb thread-init race with the fix reverted; green with it.
# Same MERIDIAN_DB_* / SKIP behaviour as db-test (inert without a live MariaDB).
add_executable(meridian-db-concurrency-test test/db_concurrency_test.cpp)
target_link_libraries(meridian-db-concurrency-test PRIVATE meridian::db Threads::Threads)
set_target_properties(meridian-db-concurrency-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME db-concurrency COMMAND meridian-db-concurrency-test)
