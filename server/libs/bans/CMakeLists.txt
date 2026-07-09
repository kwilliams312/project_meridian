# SPDX-License-Identifier: Apache-2.0
#
# meridian-bans — moderation enforcement model (OPS-02c, #419; epic #21).
# A socket-free data-access library over the ban/mute tables (auth DB
# account_ban/ip_ban/character_ban + characters DB character_mute), shared by
# authd (login refusal), worldd (session refusal + chat-mute enforcement + GM
# `.ban`/`.mute` issuance), and operator tooling. Clean-room from the SAD §4.1/
# §4.2 + PRD §4-M1/§6; no GPL source consulted (CONTRIBUTING.md).

add_library(meridian-bans STATIC
  src/bans.cpp
)
add_library(meridian::bans ALIAS meridian-bans)

target_include_directories(meridian-bans
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(meridian-bans
  PUBLIC
    meridian::db   # parameterized MariaDB access (#75)
)
target_compile_features(meridian-bans PUBLIC cxx_std_20)
set_target_properties(meridian-bans PROPERTIES CXX_EXTENSIONS OFF)

# --- Unit + integration test (OPS-02c, #419). --------------------------------
# The PURE half (ip_of_peer, duration parse, kind parse) always runs. The
# DB-backed half (active-ban queries + issuance + expiry over the real ban
# tables) runs only when MERIDIAN_DB_* is set and SKIPs (exit 0) otherwise, so it
# is inert in the plain server ctest and runs for real in the DB-backed job.
add_executable(meridian-bans-test test/bans_test.cpp)
target_link_libraries(meridian-bans-test PRIVATE meridian::bans meridian::db)
set_target_properties(meridian-bans-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME bans COMMAND meridian-bans-test)
