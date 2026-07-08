# SPDX-License-Identifier: Apache-2.0
#
# worldd — shard worker / map simulation daemon (server SAD §2.5; issues #82/#83).
#
# The IF-2 opcode dispatcher + world-process scaffold (world_dispatch) plus the
# IF-3 session establishment (world_session — grant validation + AEAD, #84)
# composes four merged libs:
#   meridian::core  — logging / version (#68)
#   meridian::net   — TLS 1.3 Session + IF-2 length framing (M0 transport, #77)
#   meridian::proto — world.fbs FlatBuffers codegen (#67)
#   meridian::db    — MariaDB Connection + prepared statements (#75) — grant
#                     consume + enter-world character load (#84)
# Split as a library (world-dispatch) + the daemon (worldd) + integration tests,
# mirroring authd's structure so the tests can drive the exact serve/dispatch
# path the daemon runs (not buried in main()). Clean-room from the SAD; no GPL
# source consulted (CONTRIBUTING.md).

find_package(Threads REQUIRED)

# --- Dispatcher + world-process scaffold + IF-3 session as a library, so the
#     integration tests can stand up the same serve loop the daemon runs. -------
add_library(world-dispatch STATIC
  world_dispatch.cpp
  world_session.cpp
  world_boot.cpp
  movement_validation.cpp
  aoi_grid.cpp
  combat_unit.cpp
  world_state.cpp
  active_sessions.cpp
  ability_store.cpp
)
add_library(meridian::world-dispatch ALIAS world-dispatch)
target_include_directories(world-dispatch
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(world-dispatch
  PUBLIC
    meridian::core
    meridian::net
    meridian::proto
    meridian::db
    meridian::characters  # char-management handlers: list/create/delete CRUD (#286 / D-35)
    meridian::metrics  # OPS-05 session/opcode/movement/tick/disconnect metrics (#164)
    meridian::trace    # OPS-05 session-flow trace: the worldd.enter_world span (#166)
    Threads::Threads
)
target_compile_features(world-dispatch PUBLIC cxx_std_20)
set_target_properties(world-dispatch PROPERTIES CXX_EXTENSIONS OFF)

# --- The daemon. --------------------------------------------------------------
add_executable(worldd
  main.cpp
)
target_link_libraries(worldd
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd PROPERTIES CXX_EXTENSIONS OFF)

# --- End-to-end integration test (IT-M0 world dispatch path). ------------------
# Self-contained: it generates a throwaway self-signed cert, stands the real TLS
# listener + world serve loop on an ephemeral port, and drives a TLS client that
# sends framed IF-2 messages. No DB or external service, so it always runs in the
# plain `ctest` (unlike the DB-gated authd test).
add_executable(worldd-dispatch-test
  test/world_dispatch_test.cpp
)
target_link_libraries(worldd-dispatch-test
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-dispatch-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-dispatch COMMAND worldd-dispatch-test)

# --- IF-3 session establishment integration test (IT-M0 enter-world; #84). -----
# The AEAD seal/open/tamper checks are PURE (no DB) and always run. The grant
# validation + single-use consume checks need a live MariaDB with the auth schema
# loaded (0001_init_auth.up.sql); the test reads MERIDIAN_DB_* env and SKIPS the
# DB half (exit 0) when none are set — so it is safe in the plain server ctest
# (AEAD only) and runs the full grant flow only in the worldd-session CI job (or
# locally with env set). It seeds a session_grant row exactly as authd writes it,
# stands the real TLS listener + the REAL WORLD_HELLO handler on an ephemeral
# port, and asserts HandshakeOk + single-use consume + reject on replay/expired/
# unknown.
add_executable(worldd-session-test
  test/world_session_test.cpp
)
target_link_libraries(worldd-session-test
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-session-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-session COMMAND worldd-session-test)

# --- Grant-lifecycle UNIT test on validate_and_consume_grant (#81). ------------
# Drives validate_and_consume_grant() DIRECTLY (no wire), so it can assert the
# GrantReject CLASSIFICATION the wire cannot expose — every grant failure is one
# opaque GRANT_INVALID Disconnect there (no client oracle), so worldd-session-test
# cannot tell WHY a grant was rejected. This pins the create/replay/expired/
# wrong-realm/unknown branches over a real MariaDB. Needs the auth schema loaded;
# reads MERIDIAN_DB_* env and SKIPS (exit 0) when none are set, so it is inert in
# the plain server ctest and runs for real only in the worldd-session CI job (or
# locally with env set).
add_executable(worldd-grant-test
  test/grant_lifecycle_test.cpp
)
target_link_libraries(worldd-grant-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-grant-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-grant COMMAND worldd-grant-test)

# --- Single active session per account UNIT test (#326). ----------------------
# PURE (no DB, no socket, no session): drives ActiveSessionRegistry directly to
# prove the "1 login" concurrency guarantee — a 2nd login for an account kicks the
# 1st (kick-old), the token compare-and-remove stops a kicked-old teardown from
# evicting its replacement, and the registry stays consistent under concurrent
# admit/release. Always runs in the plain server ctest (no MERIDIAN_DB_* needed);
# the DB-backed WORLD_HELLO wiring is exercised end-to-end by the worldd-session job.
add_executable(worldd-active-session-test
  test/active_sessions_test.cpp
)
target_link_libraries(worldd-active-session-test
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-active-session-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-active-session COMMAND worldd-active-session-test)

# --- Single active session per account END-TO-END test (#326). ----------------
# Drives TWO real TLS clients logging in for the SAME account against the real
# serve loop + WORLD_HELLO handler: the 2nd login KICKS the 1st (kick-old), the
# 1st receives Disconnect{KICKED}, and the active-session registry + AoI world
# both hold exactly one session — the #326 acceptance proven at the wire. Needs a
# live MariaDB with the auth schema (seeds two session_grant rows for one account);
# reads MERIDIAN_DB_* and SKIPS (exit 0) when none are set, so it is inert in the
# plain server ctest and runs for real in the worldd-session CI job.
add_executable(worldd-single-session-it
  test/single_session_it.cpp
)
target_link_libraries(worldd-single-session-it
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-single-session-it PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-single-session COMMAND worldd-single-session-it)

# --- Movement intake + validation v0 UNIT test (IT-M0 movement; #86). ---------
# PURE (no DB, no socket, no session): exercises the movement validator, the
# sliding-window speed check, the rate-class intake gate, and the golden
# cross-track fixture directly. It always runs in the plain server ctest (no
# MERIDIAN_DB_* needed) — the "prefer a pure function so it is DB-free
# unit-testable" ask (#86). Drift in any #101 shared constant flips a golden
# fixture vector and fails this test loudly (movement-spike.md §4 trip-wire).
add_executable(worldd-movement-test
  test/movement_validation_test.cpp
)
target_link_libraries(worldd-movement-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-movement-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-movement COMMAND worldd-movement-test)

# --- Grid / AoI engine v0 UNIT test (IT-M0 AoI core; #87). --------------------
# PURE (no DB, no socket, no session): exercises the AoiGrid spatial index + the
# interest-set query (enter / leave / hysteresis / membership / cell indexing)
# directly. It always runs in the plain server ctest (no MERIDIAN_DB_* needed) —
# the "pure grid+interest-set logic so the core runs in the plain server ctest"
# ask (#87). The DB-backed two-session relay proof is worldd-relay-test (below).
add_executable(worldd-aoi-test
  test/aoi_grid_test.cpp
)
target_link_libraries(worldd-aoi-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-aoi-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-aoi COMMAND worldd-aoi-test)

# --- Combat ability/effect data model + store UNIT test (CMB-01 foundation; #343).
# PURE (no DB, no socket, no session): drives the AbilityStore load + O(1) lookup
# and the placeholder ability set's shape directly. It always runs in the plain
# server ctest (no MERIDIAN_DB_* needed) — the ability model is an in-memory
# template store with no DB dependency at M1; the mcc/world-DB loader that fills it
# from IF-4 is epic #28 (mcc v1), a separate story. This is the "unit tests for
# load + lookup + the placeholder set's shape" ask (#343).
add_executable(worldd-ability-test
  test/ability_store_test.cpp
)
target_link_libraries(worldd-ability-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-ability-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-ability COMMAND worldd-ability-test)

# --- Combat Unit model UNIT test (CMB-01 foundation; #342). -------------------
# PURE (no DB, no socket): exercises the WorldObject→Unit→{Player,Creature}
# hierarchy + lifecycle (spawn, damage→health, death transition, heal/resurrect,
# resource spend, placeholder stats) AND the WorldState wiring (a session's Unit
# is reachable via unit_for_slot and takes damage / dies). Always runs in the
# plain server ctest (no MERIDIAN_DB_* needed) — the model must be DB-free
# unit-testable (#342).
add_executable(worldd-combat-unit-test
  test/combat_unit_test.cpp
)
target_link_libraries(worldd-combat-unit-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-combat-unit-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-combat-unit COMMAND worldd-combat-unit-test)

# --- Two-session AoI movement RELAY integration test (IT-M0 see-each-other-move;
#     #87). ---------------------------------------------------------------------
# The IT-M0 capstone: TWO real TLS clients connect + HandshakeOk + enter world
# near each other; A moves (a legal MovementIntent) -> B receives A's EntityEnter
# then EntityUpdate at A's new position (and vice versa); A then moves far out of
# B's AoI radius -> B receives EntityLeave for A. Needs a live MariaDB with the
# auth schema loaded (0001_init_auth.up.sql): two real sessions consume two
# session_grant rows. Reads MERIDIAN_DB_* env and SKIPS (exit 0) when none are
# set — inert in the plain server ctest, runs for real only in the worldd-session
# CI job (mariadb service) or locally with env set.
add_executable(worldd-relay-test
  test/world_relay_test.cpp
)
target_link_libraries(worldd-relay-test
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-relay-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-relay COMMAND worldd-relay-test)

# --- World-DB boot: manifest verify UNIT test (IT-M0 content contract; #89). ---
# PURE (no DB, no socket): exercises verify_world_manifest directly — a good
# manifest is OK, a missing/empty manifest hard-fails, a malformed content hash
# hard-fails, a schema-version mismatch hard-fails, and a pinned-hash mismatch is
# a soft warn (bootable). It always runs in the plain server ctest (no
# MERIDIAN_* env needed) — the "pure hash/verify logic must be DB-free
# unit-testable" ask (#89). The DB-backed boot proof is worldd-boot-db-test.
add_executable(worldd-boot-test
  test/world_boot_test.cpp
)
target_link_libraries(worldd-boot-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-boot-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-boot COMMAND worldd-boot-test)

# --- Character-management over the session INTEGRATION test (#286 / D-35). -----
# Drives CHAR_LIST/CREATE/DELETE over a REAL authenticated world session (TLS +
# the real serve loop + the real handlers, backed by the meridian-characters
# CRUD). Needs a live MariaDB with the auth schema loaded (it seeds + consumes a
# session_grant via the real WORLD_HELLO handler) and CREATE-TABLE-IF-NOT-EXISTS
# the `character` table in the same DB (which it points char_db at). Proves the
# #286 acceptance list end-to-end: pre-auth rejection, list-empty -> create ->
# list-shows-it, duplicate/invalid-race/invalid-class rejection, ownership-scoped
# delete (another account's character is REFUSED, the row survives), own-delete
# succeeds. Reads MERIDIAN_DB_* env and SKIPS (exit 0) when none are set — inert in
# the plain server ctest, runs for real only in the worldd-session CI job (or
# locally, scripts/dev/test.sh --db).
add_executable(worldd-char-mgmt-test
  test/char_mgmt_test.cpp
)
target_link_libraries(worldd-char-mgmt-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    Threads::Threads
)
set_target_properties(worldd-char-mgmt-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-char-mgmt COMMAND worldd-char-mgmt-test)

# --- World-DB boot: DB-backed boot INTEGRATION test (IF-4; #89). ---------------
# Loads the real world DDL (schema/sql/world/*.sql) into a live MariaDB, seeds a
# world_manifest row, and asserts read_world_manifest + boot_world_db over a real
# meridian::db::Connection: a valid manifest boots OK (verdict kOk, resolved
# version/hash logged); a truncated manifest (deleted row) fails-fast
# (kMissingManifest); a schema-version bump fails-fast (kSchemaMismatch). Needs a
# live MariaDB with the world DDL loaded; reads MERIDIAN_WORLDDB_* env and SKIPS
# (exit 0) when none are set — inert in the plain server ctest, runs for real only
# in the worldd-session CI job (mariadb service) or locally with env set.
add_executable(worldd-boot-db-test
  test/world_boot_db_test.cpp
)
target_link_libraries(worldd-boot-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-boot-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-boot-db COMMAND worldd-boot-db-test)
