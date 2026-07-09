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
  area_triggers.cpp
  combat_unit.cpp
  creature_ai.cpp
  combat_resolver.cpp
  aura_container.cpp
  death_state.cpp
  leveling.cpp
  quest_def.cpp
  quest_log.cpp
  db_content_store.cpp
  loot_registry.cpp
  world_state.cpp
  active_sessions.cpp
  ability_store.cpp
  map_tick.cpp
  movement_damage.cpp
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
    meridian::items    # ITM-01 item model + inventory/currency — quest rewards/collect (#371) + loot transfer (#369)
    meridian::loot     # ITM-02 loot tables + rolls + corpse loot sessions (#369)
    meridian::vendor   # ECO-01 vendor buy/sell/buyback handlers (#370) — pulls meridian::items
    meridian::npc      # NPC-01/02 gossip menus + trainers (#372) — pulls meridian::items

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

# --- Chat router UNIT test (SOC-01; #367). ------------------------------------
# PURE (no DB, no socket, no session): drives WorldState's chat routing directly
# with CAPTURING egress sinks (the EgressFn is a std::function, so a test lambda
# records exactly the frames the router emits). Proves the story's acceptance
# list deterministically — say reaches in-range only, yell reaches wider, whisper
# routes to the named target across sessions (and TARGET_OFFLINE for an unknown
# name), zone broadcasts to all — plus the OPS-03 chat rate class (ChatIntake).
# Always runs in the plain server ctest (no MERIDIAN_DB_* needed).
add_executable(worldd-chat-test
  test/world_chat_test.cpp
)
target_link_libraries(worldd-chat-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-chat-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-chat COMMAND worldd-chat-test)

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

# --- Creature mob AI UNIT test (issues #347 + #348, CMB-02). -------------------
# PURE (no DB, no socket): drives the CreatureAi module directly — threat-ordered
# target selection, aggro radius by level delta, leash → evade → full-heal, respawn
# timing, and waypoint patrol progression + movement output. Deterministic (tick()
# is a pure function of dt/targets/state), so it always runs in the plain server
# ctest — the seeded/deterministic ask (#347/#348).
add_executable(worldd-creature-ai-test
  test/creature_ai_test.cpp
)
target_link_libraries(worldd-creature-ai-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-creature-ai-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-creature-ai COMMAND worldd-creature-ai-test)

# --- Combat resolver UNIT test (CMB-01 #344 + #345). --------------------------
# PURE (no DB, no socket, no session): drives the combat resolver core directly —
# the attack-table classifiers at each band boundary, seeded-RNG damage/heal
# application + death, faction/range/LoS target validation, and the GCD + cast
# lifecycle (accept/reject decisions, interrupt, pushback, cast completion). All
# rolls use a seeded CombatRng so the outcomes are deterministic (SAD §2.5). Always
# runs in the plain server ctest (no MERIDIAN_DB_* needed) — the resolver is a pure
# core over the #342 Unit model + the #343 AbilityStore.
add_executable(worldd-combat-resolver-test
  test/combat_resolver_test.cpp
)
target_link_libraries(worldd-combat-resolver-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-combat-resolver-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-combat-resolver COMMAND worldd-combat-resolver-test)

# --- Combat aura container UNIT test (CMB-01 #346). ---------------------------
# PURE (no DB, no socket, no session, no clock): drives AuraContainer directly with
# a seeded CombatRng and an explicit dt — apply/refresh/expire, periodic DoT/HoT
# tick cadence + damage/heal application (via Unit::apply_damage/apply_healing),
# stat-modifier apply/remove/expiry, and the three stacking rules (refresh-duration,
# stack-count, independent-per-caster), plus a periodic-kill/host-death path and
# seed determinism. Always runs in the plain server ctest (no MERIDIAN_DB_* needed)
# — the aura container is a pure core over the #342 Unit + #343 effect model. The
# 20 Hz tick-loop wiring + golden scenarios are #349.
add_executable(worldd-aura-test
  test/aura_container_test.cpp
)
target_link_libraries(worldd-aura-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-aura-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-aura COMMAND worldd-aura-test)

# --- Combat ability-use WIRE round-trip integration test (CMB-01 #344, D-10). -
# Self-contained (no DB): stands the REAL TLS listener + world serve loop on an
# ephemeral port and drives a TLS client through the ability-use path. A test-
# installed WORLD_HELLO handler promotes the connection in-world (DB-free) so the
# CAST_REQUEST handler runs against a real spawned Unit. Proves the D-10 wire
# round-trip end-to-end: CAST_REQUEST -> CastStart (ACCEPT, cast-time ability) and
# CAST_REQUEST -> CastFailed (REJECT: unknown ability + wrong faction), so the
# client's optimistic GCD/cast confirms or rolls back within one RTT. Always runs
# in the plain server ctest (like worldd-dispatch-test).
add_executable(worldd-combat-wire-test
  test/combat_wire_test.cpp
)
target_link_libraries(worldd-combat-wire-test
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-combat-wire-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-combat-wire COMMAND worldd-combat-wire-test)

# --- Quest / loot / gossip / trainer WIRE round-trip test (issue #388). --------
# Self-contained (no DB): stands the REAL TLS listener + world serve loop on an
# ephemeral port; a test-installed WORLD_HELLO handler promotes the connection
# in-world (DB-free) with a quest log + synthetic class/level, and the test seeds
# the shared loot registry with a corpse. Proves the QST-01 / ITM-02 / NPC-01/02
# opcode families are LIVE on the session dispatch path — quest accept + log
# snapshot, gossip menu (state-gated) + a pushed trainer list, loot window open +
# release, and the typed rejections (unknown/already-active quest, wrong giver,
# unknown/foreign corpse, pre-handshake close). The DB-asserting flows (loot ->
# inventory, turn-in reward money, trainer copper debit) are the DB-backed
# worldd-quest-loot-npc test. Always runs in the plain server ctest.
add_executable(worldd-quest-loot-npc-wire-test
  test/quest_loot_npc_wire_test.cpp
)
target_link_libraries(worldd-quest-loot-npc-wire-test
  PRIVATE
    meridian::world-dispatch
    meridian::npc
    Threads::Threads
)
set_target_properties(worldd-quest-loot-npc-wire-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-quest-loot-npc-wire COMMAND worldd-quest-loot-npc-wire-test)

# --- Quest / loot / gossip / trainer LIVE-SESSION DB integration test (#388). --
# The acceptance bar: drives the QST-01 / ITM-02 / NPC-01/02 opcodes over a REAL
# TLS session against a REAL MariaDB and asserts the DURABLE side effects — looted
# ore rows land in the character's DB inventory, loot money + quest reward + trainer
# debit all move character.money over the wire, and out-of-phase / wrong-looter
# requests get typed rejects. CREATE-TABLE-IF-NOT-EXISTS the character / item /
# inventory tables (mirrors 0001_init_characters.up.sql) so the same DB is the
# characters DB. Reads MERIDIAN_DB_* and SKIPS (exit 0) when none are set — inert in
# the plain server ctest, real only in the worldd-session CI job / test.sh --db.
add_executable(worldd-quest-loot-npc-db-test
  test/quest_loot_npc_db_it.cpp
)
target_link_libraries(worldd-quest-loot-npc-db-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    meridian::npc
    Threads::Threads
)
set_target_properties(worldd-quest-loot-npc-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-quest-loot-npc-db COMMAND worldd-quest-loot-npc-db-test)

# --- Area triggers + POI discovery UNIT test (WLD-01/03; #368). ---------------
# PURE (no DB, no socket, no TLS): exercises BOTH layers directly —
#   (1) the AreaTriggerSet core (point-in-box membership, enter/leave fire ONCE
#       per crossing, POI discovery marks + fires once, re-entry does NOT re-fire
#       discovery), and
#   (2) the WorldState wiring: enter() / on_movement() drive trigger evaluation and
#       route a POI_DISCOVERED frame through the mover's egress. WorldState's egress
#       is a std::function, so the test captures emitted opcodes with a plain lambda
#       — no TLS/DB — and asserts POI_DISCOVERED fires exactly once and not on
#       re-entry, plus the server-side OnAreaTrigger hook fires per crossing.
# Deterministic (seeded volume set + scripted positions). Always runs in the plain
# server ctest (no MERIDIAN_DB_* needed).
add_executable(worldd-area-triggers-test
  test/area_triggers_test.cpp
)
target_link_libraries(worldd-area-triggers-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-area-triggers-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-area-triggers COMMAND worldd-area-triggers-test)

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

# --- World-DB CONTENT LOAD DB-backed integration test (#390). -------------------
# The acceptance bar for the content-load story: seed a world DB with the authored
# fixture content (server/worldd/test/fixtures/world_content, compiled to world-DB
# rows by `mcc emit-sql`), construct the DB-backed content stores (DbQuestStore /
# DbNpcStore / DbLootTableStore / DbVendorCatalog / DbTemplateStore) against it, and
# assert each returns the authored definitions behind the SAME seam the placeholder
# store implements — a quest's objectives/rewards/prereqs, an NPC's roles + quest
# refs, a loot table's entries keyed by creature (with the quest-gated drop + additive
# money), a vendor's catalog + resolved prices. Self-seeds minimal no-FK tables
# (mirroring the mcc-emitted rows) so it is deterministic without the full world DDL.
# Reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0) when none
# are set — inert in the plain server ctest, real only in the DB CI job / test.sh --db.
add_executable(worldd-content-load-db-test
  test/content_load_db_it.cpp
)
target_link_libraries(worldd-content-load-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-content-load-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-content-load-db COMMAND worldd-content-load-db-test)

# --- IT-M1 10-quest CHAIN content consistency + load test (#394). ---------------
# The content half of the epic-#20 exit criterion. Unlike worldd-content-load-db
# (which self-seeds fixture rows), this test runs the REAL pipeline end to end: it
# invokes `mcc emit-sql` on content/core, loads the real world DDL + the emitted DML
# into a throwaway MariaDB, constructs the #390 DB stores over it, and asserts the
# authored 10-quest chain is internally consistent — all 10 quests load, the
# prerequisite graph is a valid DAG, every kill/collect/deliver target + explore POI
# resolves, every reward + giver/turn-in NPC resolves, and the loot tables actually
# drop the collect-objective items. TOOL- + ENV-GUARDED: needs a MariaDB
# (MERIDIAN_WORLDDB_*/MERIDIAN_DB_*), a mariadb/mysql client, and an mcc binary;
# SKIPS (exit 0) otherwise, so it is inert in the plain server ctest and runs for
# real under test.sh --db / the DB CI job. Paths (content root, world DDL, default
# mcc binary) are baked in as compile definitions; MERIDIAN_MCC_BIN overrides the
# binary at runtime.
add_executable(worldd-itm1-chain-load-test
  test/itm1_chain_load_it.cpp
)
target_link_libraries(worldd-itm1-chain-load-test
  PRIVATE
    meridian::world-dispatch
)
target_compile_definitions(worldd-itm1-chain-load-test PRIVATE
  ITM1_CONTENT_DIR="${CMAKE_SOURCE_DIR}/../content"
  ITM1_WORLD_DDL_DIR="${CMAKE_SOURCE_DIR}/../schema/sql/world"
  ITM1_MCC_BIN="${CMAKE_BINARY_DIR}/../mcc/mcc")
set_target_properties(worldd-itm1-chain-load-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-itm1-chain-load COMMAND worldd-itm1-chain-load-test)

# --- Combat tick + seeded GOLDEN sim-harness scenarios (CMB-01/02; #349). ------
# The epic-#18 capstone. PURE (no DB, no socket): drives the MapTick orchestrator
# (map_tick.{h,cpp}) — the single-threaded per-map tick in SAD §2.5 phase order —
# through three SEEDED, byte-stable golden scenarios that prove the combat epic
# end-to-end:
#   A. ability use → GCD/accept → cast completes on a later tick → attack-table
#      roll → damage → DEATH (closes the #354 cast-completion + resource-spend seam);
#   B. an instant-applied DoT TICKING over several ticks via the aura container;
#   C. a creature aggro → leash → EVADE → full-heal → resume patrol → death →
#      RESPAWN cycle driven by creature_ai in the AI phase.
# Every roll draws from a fixed-seed CombatRng, so the whole event stream is
# deterministic — the test asserts each scenario's log against an embedded golden
# blob, so ANY drift in an attack-table band, a damage/heal/aura formula, an AI
# transition, or the tick PHASE ORDER flips a golden line and fails CI. Always runs
# in the plain server ctest (no MERIDIAN_* env needed).
add_executable(worldd-combat-golden-test
  test/combat_golden_test.cpp
)
target_link_libraries(worldd-combat-golden-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-combat-golden-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-combat-golden COMMAND worldd-combat-golden-test)

# --- XP curve / leveling UNIT test (CHR-03 #360). -----------------------------
# PURE (no DB, no socket, no clock, no RNG): drives the leveling curve directly —
# xp_to_next_level monotonicity, xp_for_kill grey-mob falloff, and grant_xp
# roll-over (including multi-level awards + the level cap). Deterministic, so it
# always runs in the plain server ctest — the seeded/deterministic ask (#360).
add_executable(worldd-leveling-test
  test/leveling_test.cpp
)
target_link_libraries(worldd-leveling-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-leveling-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-leveling COMMAND worldd-leveling-test)

# --- Player death state machine UNIT test (CMB-03 #359). ----------------------
# PURE (no DB, no socket): drives DeathStateMachine directly — on_death spawns a
# corpse + enters kCorpse, auto-release timer + requested release → kGhost,
# corpse-run distance gating for resurrect, and resurrect despawns the corpse +
# restores the health %. Deterministic (dt passed in), so it always runs in the
# plain server ctest.
add_executable(worldd-death-state-test
  test/death_state_test.cpp
)
target_link_libraries(worldd-death-state-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-death-state-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-death-state COMMAND worldd-death-state-test)

# --- Quest state machine UNIT test (QST-01 #371). -----------------------------
# PURE (no DB, no socket, no clock, no RNG): drives the quest state machine
# (quest_def.h placeholder chain + quest_log.h) directly — accept gating
# (level + prerequisite), the four objective sources (kill / collect / deliver /
# explore), can't-turn-in-incomplete, wrong-NPC turn-in, and the reward grant
# (items minted into the inventory + XP/copper amounts). Also drives the kill
# objective END-TO-END through the MapTick on_unit_died hook (the #359/#365 seam).
# Deterministic, so it always runs in the plain server ctest (no MariaDB).
add_executable(worldd-quest-test
  test/quest_test.cpp
)
target_link_libraries(worldd-quest-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-quest-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-quest COMMAND worldd-quest-test)

# --- Death + XP END-TO-END through the MapTick hook (CMB-03 #359 + CHR-03 #360).
# PURE (no DB, no socket): drives the MapTick orchestrator through the SINGLE
# "unit died" hook both stories share. Two seeded/deterministic scenarios:
#   1. a player dies (via the resolver) → corpse spawns → auto-release to ghost →
#      corpse-run → resurrect (health restored, corpse despawned);
#   2. a player kills creatures and levels 1→5, with stat growth applied from the
#      level curve at each level-up (fully server-authoritative).
# Always runs in the plain server ctest (no MERIDIAN_* env needed).
add_executable(worldd-death-xp-test
  test/death_xp_test.cpp
)
target_link_libraries(worldd-death-xp-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-death-xp-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-death-xp COMMAND worldd-death-xp-test)

# --- Loot on creature death END-TO-END through the MapTick hook (ITM-02 #369). --
# PURE (no DB, no socket): drives the MapTick orchestrator — a player kills a
# creature that carries a loot table → the map rolls the table into a loot session
# on the corpse (SEEDED, so the roll is reproducible for a map seed and does NOT
# perturb the combat rng_), and a client loots slots into a real items::Inventory
# through the session's server-side validation (ownership / in-range / not-already-
# looted / quest gate). Proves the story's acceptance list at the integration seam:
# deterministic roll for a seed, loot-to-inventory transfers ownership, quest-gated
# drop only for eligible players, and no double-loot. Always runs in the plain
# server ctest (no MERIDIAN_* env needed).
add_executable(worldd-loot-test
  test/loot_session_it.cpp
)
target_link_libraries(worldd-loot-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-loot-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-loot COMMAND worldd-loot-test)

# --- Movement-derived (fall + swim/breath) damage unit test (issue #362, CHR-02
# full). Exercises the PURE MovementDamageState evaluator directly (fall above the
# safe threshold → scaled damage, none below; breath depletion underwater → drowning
# ticks; refill on surfacing; water breaks a fall) AND the MapTick combat-phase
# WIRING (a landing / drowning applies HP via Unit::apply_damage). Deterministic (no
# RNG, no clock) — always runs in the plain server ctest (no MERIDIAN_* env needed).
add_executable(worldd-movement-damage-test
  test/movement_damage_test.cpp
)
target_link_libraries(worldd-movement-damage-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-movement-damage-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-movement-damage COMMAND worldd-movement-damage-test)
