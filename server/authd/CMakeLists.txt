# SPDX-License-Identifier: Apache-2.0
#
# authd — login / realm-list / session-grant daemon (server SAD §2.1; issue #79).
# The IF-1 login state machine (login_session) composes FIVE merged libs:
#   meridian::net   — TLS 1.3 Session + IF-1 framing (#77)
#   meridian::proto — auth.fbs FlatBuffers codegen (#67)
#   meridian::srp   — SRP-6a ServerSession (#78)
#   meridian::db    — MariaDB Connection + prepared statements (#75)
# and (in the integration test only) meridian::account for account setup (#80).
# Clean-room from the SAD; no GPL source consulted (CONTRIBUTING.md).

find_package(Threads REQUIRED)

# --- Login state machine as a library, so the integration test can drive the
#     exact flow the daemon runs (not buried in main()). -----------------------
add_library(authd-login STATIC
  login_session.cpp
)
add_library(meridian::authd-login ALIAS authd-login)
target_include_directories(authd-login
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(authd-login
  PUBLIC
    meridian::core     # OPS-05 structured audit stream: ban-rejection records (#92/#419)
    meridian::net
    meridian::proto
    meridian::srp
    meridian::db
    meridian::bans     # OPS-02c account/IP ban lookup at login (#419)
    meridian::metrics  # OPS-05 auth-outcome + SRP-timing metrics (#164)
    meridian::trace    # OPS-05 session-flow trace: the authd.login span (#166)
)
target_compile_features(authd-login PUBLIC cxx_std_20)
set_target_properties(authd-login PROPERTIES CXX_EXTENSIONS OFF)

# --- The daemon. --------------------------------------------------------------
add_executable(authd
  main.cpp
)
target_link_libraries(authd
  PRIVATE
    meridian::authd-login
    Threads::Threads
)
set_target_properties(authd PROPERTIES CXX_EXTENSIONS OFF)

# --- End-to-end integration test (IT-M0 auth path). ---------------------------
# Needs a live MariaDB (auth schema loaded). Reads MERIDIAN_DB_* env and SKIPS
# (exit 0) when none are set, so it is inert in the plain server build's ctest
# and runs for real only in the authd CI job (or locally with env set). It
# stands the real TLS listener + login state machine on an ephemeral port, drives
# a full IF-1 login over TLS 1.3 with a real SRP client, and asserts the grant is
# written, single-use, and that a wrong password is rejected with no grant.
add_executable(authd-login-test
  test/login_flow_test.cpp
)
target_link_libraries(authd-login-test
  PRIVATE
    meridian::authd-login
    meridian::account
    meridian::srp
    Threads::Threads
)
set_target_properties(authd-login-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME authd-login COMMAND authd-login-test)

# --- Ban enforcement at login (OPS-02c, #419). --------------------------------
# Stands up the real TLS listener + run_login against a live MariaDB and asserts:
# a non-banned account authenticates; a banned account is refused (ACCOUNT_BANNED)
# with no grant; an EXPIRED ban does not block; a banned source IP is refused at
# ServerHello. Reads MERIDIAN_DB_* + SKIPs (exit 0) when unset (inert in the
# DB-free ctest; runs for real in the DB-backed job).
add_executable(authd-ban-login-test
  test/ban_login_test.cpp
)
target_link_libraries(authd-ban-login-test
  PRIVATE
    meridian::authd-login
    meridian::account
    meridian::bans
    meridian::srp
    Threads::Threads
)
set_target_properties(authd-ban-login-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME authd-ban-login COMMAND authd-ban-login-test)
