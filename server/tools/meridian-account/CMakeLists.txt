# SPDX-License-Identifier: Apache-2.0
#
# meridian-account — the M0 account-creation CLI (ACC-03; issue #80).
# Composes meridian-srp (SRP-6a make_verifier, 2048-bit + SHA-256 defaults) and
# meridian-db (Connection + prepared statements) to register an account into the
# auth DB `account` table (server SAD §2.1, §4.1). Clean-room, Apache-2.0.

# Account-creation code path as a small library, so the integration test can
# drive the exact path the CLI uses.
add_library(meridian-account-lib STATIC
  src/account.cpp
)
add_library(meridian::account ALIAS meridian-account-lib)
target_include_directories(meridian-account-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-account-lib PUBLIC meridian::srp meridian::db)
target_compile_features(meridian-account-lib PUBLIC cxx_std_20)
set_target_properties(meridian-account-lib PROPERTIES CXX_EXTENSIONS OFF)

# The CLI binary.
add_executable(meridian-account src/main.cpp)
target_link_libraries(meridian-account PRIVATE meridian::account)
set_target_properties(meridian-account PROPERTIES CXX_EXTENSIONS OFF)

# Integration test — needs a live MariaDB. Reads MERIDIAN_DB_* env and SKIPS
# (exit 0) when none are set, so it is inert in the plain server build's ctest
# and runs for real only in the account-CLI CI job (or locally with env set).
# It creates an account through the library code path, reads back {salt,
# verifier}, and runs a full SRP-6a exchange to prove the credential works.
add_executable(meridian-account-test test/account_test.cpp)
target_link_libraries(meridian-account-test PRIVATE meridian::account meridian::srp)
set_target_properties(meridian-account-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME account COMMAND meridian-account-test)
