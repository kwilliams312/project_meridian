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
    equipment_service.cpp
  world_session.cpp
  world_boot.cpp
  movement_validation.cpp
  movement_audit.cpp
  rate_class.cpp
  economy_sanity.cpp
  gm_command.cpp
  aoi_grid.cpp
  area_triggers.cpp
  combat_unit.cpp
  creature_ai.cpp
  combat_resolver.cpp
  aura_container.cpp
  effective_stats.cpp
  class_kernel.cpp
  talent_catalog.cpp
  death_state.cpp
  leveling.cpp
  quest_def.cpp
  quest_log.cpp
  quest_credit.cpp
  vitals_egress.cpp
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
    meridian::bans     # OPS-02c ban/mute enforcement + .ban/.mute issuance (#419)
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

# --- Self-vitals-at-ENTER_WORLD live-session test (UI-01 HUD contract; #439). --
# Drives ONE real TLS client through the real serve loop + WORLD_HELLO + ENTER_WORLD
# handlers: on spawn the session receives an authoritative VITALS_UPDATE for its OWN
# character (health/max, power/max + type, level) BEFORE it sends any combat — the
# #438 gap (blank HUD player frame until the first delta) this closes. Needs a live
# MariaDB with the auth schema (seeds an account + owned character + grant); reads
# MERIDIAN_DB_* and SKIPS (exit 0) when none are set, so it is inert in the plain
# server ctest and runs for real in the worldd-session CI job / test.sh --db. The
# DB-free vitals broadcast LOGIC is proven by worldd-vitals-test.
add_executable(worldd-self-vitals-it
  test/self_vitals_enter_it.cpp
)
target_link_libraries(worldd-self-vitals-it
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-self-vitals-it PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-self-vitals COMMAND worldd-self-vitals-it)

# --- Ban + mute enforcement END-TO-END test (OPS-02c, #419). ------------------
# Drives real TLS clients against the real serve loop: a BANNED account's WorldHello
# is refused (Disconnect{KICKED}); a spawned character's chat routes normally until
# it is MUTED (dropped with a "you are muted" System reply) and again once the mute
# has EXPIRED. Needs a live MariaDB with the auth schema; reads MERIDIAN_DB_* and
# SKIPS (exit 0) when none are set (inert in the plain server ctest, runs in the
# worldd-session CI job).
add_executable(worldd-ban-mute-it
  test/ban_mute_it.cpp
)
target_link_libraries(worldd-ban-mute-it
  PRIVATE
    meridian::world-dispatch
    meridian::bans
    Threads::Threads
)
set_target_properties(worldd-ban-mute-it PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-ban-mute COMMAND worldd-ban-mute-it)

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

# --- Per-opcode RATE CLASS UNIT test (OPS-03b; #421). -------------------------
# PURE (no DB, no socket, no session): drives the RateLimiter + rate_class table
# directly with an injected clock, covering ≥2 classes (a chat flood vs a normal
# chat rate; a move flood; the window slide) + the anti-cheat audit record the
# dispatch emits on a flood drop. Always runs in the plain server ctest.
add_executable(worldd-rate-class-test
  test/rate_class_test.cpp
)
target_link_libraries(worldd-rate-class-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-rate-class-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-rate-class COMMAND worldd-rate-class-test)

# --- Economy sanity UNIT test (OPS-03b; #421). -------------------------------
# PURE (no DB): drives the defense-in-depth economy delta checks directly — a bad
# quantity / negative price / copper over/underflow is rejected while a normal
# transaction passes — plus the economy audit record. Always runs in plain ctest.
add_executable(worldd-economy-sanity-test
  test/economy_sanity_test.cpp
)
target_link_libraries(worldd-economy-sanity-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-economy-sanity-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-economy-sanity COMMAND worldd-economy-sanity-test)

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

# --- Unit VITALS broadcast UNIT test (UI-01 HUD contract; #430). --------------
# PURE (no DB, no socket, no session): drives WorldState directly with CAPTURING
# egress sinks, so it asserts the exact EntityEnter vitals block + VITALS_UPDATE
# deltas the relay emits without standing up a TLS session. Proves the story's
# acceptance: an entity entering AoI carries correct health/power/level/name; a
# damage/heal moves health and emits the delta (to self + AoI observers); a dead
# unit reports health 0; a level-up updates level + max_health/max_power. Always
# runs in the plain server ctest (no MERIDIAN_DB_* needed).
add_executable(worldd-vitals-test
  test/world_vitals_test.cpp
)
target_link_libraries(worldd-vitals-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-vitals-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-vitals COMMAND worldd-vitals-test)

# --- EntityEnter appearance + equipped-visuals broadcast (②/T1, #538). ---------
# DB-free: drives encode_entity_enter_payload() directly to prove the additive
# visual-assembly fields — a player round-trips race/sex/appearance/equipment
# (incl. a numeric-id dye choice); an NPC (no visual) omits them entirely.
add_executable(worldd-entity-enter-visual-test
  test/entity_enter_visual_test.cpp
)
target_link_libraries(worldd-entity-enter-visual-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-entity-enter-visual-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-entity-enter-visual COMMAND worldd-entity-enter-visual-test)

# --- GM COMMAND FRAMEWORK UNIT test (OPS-02a; #417). --------------------------
# PURE (no DB, no socket, no session): drives the gm_command framework directly
# with CAPTURING reply + audit sinks (both std::function seams), so it asserts the
# exact parse / permission-gate / audit / execute path the live chat handler runs
# without standing up a TLS session. Proves the story's acceptance: `.help` from a
# GM-level session lists the available commands + audits a SUCCESS; from a player
# session it is DENIED + a GM-audit failure is emitted; an unknown command is
# rejected + audited. Always runs in the plain server ctest (no MERIDIAN_DB_*).
add_executable(worldd-gm-command-test
  test/gm_command_test.cpp
)
target_link_libraries(worldd-gm-command-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-gm-command-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-gm-command COMMAND worldd-gm-command-test)

# --- GM COMMAND SET effect INTEGRATION test (OPS-02b; #418). ------------------
# PURE (no DB, no socket, no session): drives the REAL server-side effect surfaces
# the M1 GM command set acts on — WorldState (summon / kick / setlevel) +
# SessionMovementState (the .tele/.summon force-correction + ack barrier) — with
# capturing egress sinks, so it proves each command's SERVER STATE CHANGE actually
# happens (position / world Unit level / session teardown / forced-move mailbox)
# and that a GM teleport does NOT trip the #420 anti-cheat teleport check. The
# parse/gate/audit half is proven by worldd-gm-command-test; the DB-backed
# .additem mint/persist rides the same itm path the loot/quest DB tests exercise.
# Always runs in the plain server ctest (no MERIDIAN_DB_* needed).
add_executable(worldd-gm-effect-test
  test/gm_effect_test.cpp
)
target_link_libraries(worldd-gm-effect-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-gm-effect-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-gm-effect COMMAND worldd-gm-effect-test)

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

# --- Authored hostile basic-attack integration (#784). ----------------------
add_executable(worldd-mob-basic-attack-test
  test/mob_basic_attack_test.cpp
)
target_link_libraries(worldd-mob-basic-attack-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-mob-basic-attack-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-mob-basic-attack COMMAND worldd-mob-basic-attack-test)

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

# --- LIVE quest objective crediting integration test (QST-01 event-bus; #396). -
# Proves a player's world actions advance the quests THEY accepted and emit
# QUEST_PROGRESS, per objective SOURCE, over the REAL serve/dispatch loop: a KILL
# routed through a real MapTick + the quest-credit bus (and NOT a different guid's
# quest), a DELIVER at the target NPC, and an EXPLORE via an area-trigger crossing.
# DB-free (synthetic quest content + a stub WORLD_HELLO), so it always runs in the
# plain server ctest; the COLLECT live path is the DB-backed worldd-quest-loot-npc
# test's regression guard.
add_executable(worldd-quest-live-credit-test
  test/quest_live_credit_it.cpp
)
target_link_libraries(worldd-quest-live-credit-test
  PRIVATE
    meridian::world-dispatch
    meridian::npc
    Threads::Threads
)
set_target_properties(worldd-quest-live-credit-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-quest-live-credit COMMAND worldd-quest-live-credit-test)

# --- LIVE MapTick level-up → VITALS_UPDATE egress integration test (UI-01; #437). -
# Proves that a kill routed through a REAL MapTick which LEVELS a live player up emits
# a VITALS_UPDATE carrying the new level + max_health/max_power to that player's own
# session (the HUD player frame) over the REAL serve/dispatch loop — routed through
# the SAME route_vitals_events + vitals-egress bus the world thread runs, and NOT to a
# different guid's session (per-leveler isolation). DB-free (a stub WORLD_HELLO
# promotes IN-WORLD with no grant DB), so it always runs in the plain server ctest;
# the DB-free vitals broadcast LOGIC is proven by worldd-vitals-test, the ENTER_WORLD
# wiring by worldd-self-vitals.
add_executable(worldd-vitals-levelup-it
  test/vitals_levelup_it.cpp
)
target_link_libraries(worldd-vitals-levelup-it
  PRIVATE
    meridian::world-dispatch
    Threads::Threads
)
set_target_properties(worldd-vitals-levelup-it PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-vitals-levelup COMMAND worldd-vitals-levelup-it)

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

# --- Trainer-vs-DB-content LIVE-SESSION integration test (issue #392). ---------
# The live half of #392: a real TLS session drives GOSSIP_HELLO -> TRAINER_LIST +
# TRAINER_LEARN against a world server whose NPC seam is a DbNpcStore loaded from
# seeded npc_trainer / npc_trainer_ability rows (install_content_stores) — proving a
# DB-loaded trainer works end-to-end (learn debits character.money; every rejection
# axis leaves it untouched). Reads MERIDIAN_DB_* and SKIPS (exit 0) when unset.
add_executable(worldd-trainer-db-wire-test
  test/trainer_db_wire_it.cpp
)
target_link_libraries(worldd-trainer-db-wire-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    meridian::npc
    Threads::Threads
)
set_target_properties(worldd-trainer-db-wire-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-trainer-db-wire COMMAND worldd-trainer-db-wire-test)

# --- HUD read-contracts LIVE-SESSION integration test (issue #453). ------------
# The two server->client READ contracts the HUD needs, over a real TLS session
# against a real MariaDB using the M1 PLACEHOLDER stores: ENTER_WORLD pushes an
# INVENTORY_SNAPSHOT (money + bags), a vendor GOSSIP_HELLO auto-pushes VENDOR_LIST
# (server-computed prices), and a buy/sell/buyback re-pushes the snapshot +
# VENDOR_BUYBACK_RESULT echoes buyback_slot. Reads MERIDIAN_DB_* and SKIPS when unset.
add_executable(worldd-hud-read-contracts-test
  test/hud_read_contracts_it.cpp
)
target_link_libraries(worldd-hud-read-contracts-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    meridian::npc
    Threads::Threads
)
set_target_properties(worldd-hud-read-contracts-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-hud-read-contracts COMMAND worldd-hud-read-contracts-test)

# --- KNOWN_ABILITIES enter-world LIVE-SESSION integration test (issue #457). ---
# The server->client SPELLBOOK read contract the action bar (#456) needs, over a real
# TLS session against a real MariaDB using the M1 PLACEHOLDER stores: ENTER_WORLD
# pushes a KNOWN_ABILITIES (empty for a fresh character) and a TRAINER_LEARN that
# grows the set re-pushes it with the learned ability + its AbilityStore metadata
# (cast_ms / triggers_gcd / resource / range). Reads MERIDIAN_DB_* and SKIPS when unset.
add_executable(worldd-known-abilities-test
  test/known_abilities_enter_it.cpp
)
target_link_libraries(worldd-known-abilities-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    meridian::npc
    Threads::Threads
)
set_target_properties(worldd-known-abilities-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-known-abilities COMMAND worldd-known-abilities-test)

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

# --- Cross-session character-persistence REGRESSION test (#543 / #341). ---------
# Guards the actual #341 bug ("create a character, reopen the client → gone"): a
# character created over ONE connection must be readable, owned by the same
# account, on a SEPARATELY-opened FRESH connection against the same durable DB.
# char_mgmt_test above proves the create→list round-trip WITHIN one session/handle
# (so state that only lived in that connection would still pass it); this drives
# the REAL reconnect read paths — meridian::characters::list_characters (CHAR_LIST)
# and meridian::worldd::load_owned_character (ENTER_WORLD ownership) — from a brand
# new connection to prove DURABILITY, not just session round-trip. Needs a live
# MariaDB; CREATE-TABLE-IF-NOT-EXISTS the standalone `character` table (no outgoing
# FK, §4.4) and seeds its own randomised account_id (no auth grant needed). Reads
# MERIDIAN_DB_* env and SKIPS (exit 0) when none are set — inert in the plain server
# ctest, runs for real only in the worldd-session CI job (or locally via
# scripts/dev/test.sh --db).
add_executable(worldd-char-persist-test
  test/char_persist_test.cpp
)
target_link_libraries(worldd-char-persist-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
)
set_target_properties(worldd-char-persist-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-char-persist COMMAND worldd-char-persist-test)

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

# --- Effective-stat framework DB-backed integration test (SP2.4 #694). ----------
# The MANDATORY DB-backed proof: seed the `attribute` / `class_attribute_mod` /
# `race_attribute_mod` tables (mcc emit-sql's shape) into a real MariaDB, read them
# back with load_db_attributes, and prove a character's effective stats reflect
# base + class mods + race mods loaded from the DB, a buff modifies the right
# effective attribute (FLAT and PERCENT), a DERIVED attribute (armor) computes, and
# aura expiry rolls back. Self-seeds minimal no-FK tables. Reads MERIDIAN_WORLDDB_*
# (falls back to MERIDIAN_DB_*) and SKIPS (exit 0) when none are set — inert in the
# plain server ctest, real only under test.sh --db / the DB CI job.
add_executable(worldd-effective-stats-db-test
  test/effective_stats_db_it.cpp
)
target_link_libraries(worldd-effective-stats-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-effective-stats-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-effective-stats-db COMMAND worldd-effective-stats-db-test)

# --- Class-kernel unit test (SP2.7 #697): equip-gating + category-match + role hook
# + talent application, over in-memory catalogs (no DB). Locks the pure decision
# logic the DB-backed test proves against real MariaDB rows. Always runs in the plain
# server ctest (no MERIDIAN_DB_* needed) — the kernel is pure data + arithmetic.
add_executable(worldd-class-kernel-test
  test/class_kernel_test.cpp
)
target_link_libraries(worldd-class-kernel-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-class-kernel-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-class-kernel COMMAND worldd-class-kernel-test)

# --- Equip-gating + role/talent DB-backed integration test (SP2.7 #697). ---------
# The MANDATORY DB-backed proof: seed the `equip_type` / `class` /
# `class_usable_equip_type` / `class_role` / `talent*` / `attribute` / `item_template`
# tables (mcc emit-sql's shape) into a real MariaDB, read them back with
# load_db_equip_types / load_db_class_catalog / load_db_talents / DbTemplateStore, and
# prove gate_equip ACCEPTS a proficient in-category equip, REJECTS a non-usable type
# AND a category mismatch, the Tank role amplifies threat, and a talent passive raises
# effective strength through the SP2.4 framework — all off DB rows. Self-seeds minimal
# no-FK tables. Reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS
# (exit 0) when none are set — inert in the plain server ctest, real under test.sh --db.
add_executable(worldd-equip-gating-db-test
  test/equip_gating_db_it.cpp
)
target_link_libraries(worldd-equip-gating-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-equip-gating-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-equip-gating-db COMMAND worldd-equip-gating-db-test)

# Authoritative equipment placement transaction (#802): equip, occupied-slot
# replacement, class rejection rollback, and unequip persistence in MariaDB.
add_executable(worldd-equipment-service-db-test
  test/equipment_service_db_it.cpp
)
target_link_libraries(worldd-equipment-service-db-test PRIVATE meridian::world-dispatch)
set_target_properties(worldd-equipment-service-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-equipment-service-db COMMAND worldd-equipment-service-db-test)

# --- Content SPAWN load + spawn-into-world DB-backed integration test (#486). ----
# The acceptance bar for the spawn story: seed a world DB with authored spawn_point
# rows (a friendly quest-giver NPC + a hostile creature) resolved against npc_template,
# then prove load_world_content reads them into WorldContent.spawns, WorldServer::
# install_spawns spawns each into BOTH the map tick (existence) AND the AoI relay (an
# entity), a session entering AoI receives an ENTITY_ENTER for the nearby spawns (with
# the #430 vitals + name), the gossip NPC's wire guid resolves to its npc_template id
# (the GOSSIP_HELLO seam) + is reachable by guid, and a hostile creature is present for
# a kill objective. Self-seeds minimal no-FK tables. Reads MERIDIAN_WORLDDB_* (falls
# back to MERIDIAN_DB_*) and SKIPS (exit 0) when none are set — inert in the plain
# server ctest, real only in the DB CI job / test.sh --db.
add_executable(worldd-spawn-load-db-test
  test/spawn_load_db_it.cpp
)
target_link_libraries(worldd-spawn-load-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-spawn-load-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-spawn-load-db COMMAND worldd-spawn-load-db-test)

# --- Enter-world START-ZONE SPAWN load DB-backed integration test (C8, #761). ----
# The acceptance bar for enter-as-chibi: a character entering the world spawns at
# its realm's START ZONE first graveyard (zone.schema.yaml "start_zone: spawn point
# = first graveyard"), not the D-11 placeholder. Proves load_start_zone_spawn
# resolves the start zone's ordinal-0 graveyard with the #498 Godot-Y-up -> server-
# Z-up axis conversion, ignores non-start zones + later ordinals, matches the real
# chibi Sprout-Meadow-at-origin shape, and degrades to nullopt (no start zone / no
# graveyard) so the handler keeps the placeholder. DB-LEVEL isolated (#707): a unique
# pid-keyed throwaway database seeded with only the zone/graveyard tables the loader
# reads. Reads MERIDIAN_WORLDDB_* (falls back to MERIDIAN_DB_*) and SKIPS (exit 0)
# when none are set — inert in the plain server ctest, real only in the DB CI job /
# test.sh --db.
add_executable(worldd-enter-spawn-db-test
  test/enter_spawn_db_it.cpp
)
target_link_libraries(worldd-enter-spawn-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-enter-spawn-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-enter-spawn-db COMMAND worldd-enter-spawn-db-test)

# --- Ability-catalog world-DB load + cast-resolution regression test (#481). ----
# Reproduces the UNKNOWN_ABILITY cast bug (the placeholder store does not know the
# authored ids the CAST_REQUEST handler resolves against) and proves load_db_ability_store
# reads the authored `ability`/`ability_effect`/`ability_effect_stat_mod` catalog so
# find() resolves — with the metadata pinned to mcc's content/core emit (minor_healing=1,
# pickaxe_slam=2). Self-seeds minimal no-FK tables (mirroring the mcc-emitted rows) so it
# is deterministic without the full world DDL. Reads MERIDIAN_WORLDDB_* (falls back to
# MERIDIAN_DB_*) and SKIPS (exit 0) when none are set — inert in the plain server ctest,
# real only in the DB CI job / test.sh --db.
add_executable(worldd-cast-ability-db-test
  test/cast_ability_db_it.cpp
)
target_link_libraries(worldd-cast-ability-db-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-cast-ability-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-cast-ability-db COMMAND worldd-cast-ability-db-test)

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

# --- SP5 CHIBI CLASSES boot->create->equip->cast DB-backed test (#723; epic #722). -
# The SP2-discipline runtime proof that a whole new class roster ships as PURE PACK
# DATA on the SP2 kernel with zero engine change. Runs the REAL pipeline like
# worldd-itm1-chain-load: mcc emit-sql content --pack chibi -> throwaway MariaDB
# (world DDL + emitted DML) -> load_world_content, then drives boot (the 4 chibi
# classes Warrior/Mage/Rogue/Priest, roster 1-4, load into the Roster + ClassCatalog
# with the right role + proficiencies), create (a Blue/Warrior character validated
# vs the DB roster), equip (gate_equip accepts each class's starter items, rejects a
# wrong-type item), and cast (the chibi abilities execute through MapTick —
# Warrior Crushing Blow damages + Bulwark shields, Mage Cinderburn burns, Priest Mend
# heals). Links meridian::characters for the create-vs-roster path. Same TOOL-/ENV-
# guards (MERIDIAN_WORLDDB_*/MERIDIAN_DB_* + a mariadb/mysql client + an mcc binary);
# SKIPS (exit 0) otherwise — inert in the plain server ctest, real under test.sh --db.
add_executable(worldd-chibi-classes-db-test
  test/chibi_classes_db_it.cpp
)
target_link_libraries(worldd-chibi-classes-db-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
)
target_compile_definitions(worldd-chibi-classes-db-test PRIVATE
  CHIBI_CONTENT_DIR="${CMAKE_SOURCE_DIR}/../content"
  CHIBI_WORLD_DDL_DIR="${CMAKE_SOURCE_DIR}/../schema/sql/world"
  CHIBI_MCC_BIN="${CMAKE_BINARY_DIR}/../mcc/mcc")
set_target_properties(worldd-chibi-classes-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-chibi-classes-db COMMAND worldd-chibi-classes-db-test)

# --- DB-loaded area-trigger POI -> explore crediting test (#398). ---------------
# Proves the last content-load link: the world-DB `area` (POI) rows load (via
# load_world_content) into discovery TriggerVolumes carrying the real `poi`, and a
# player crossing a DB-loaded volume credits the matching AUTHORED explore objective
# (on_explore(zone_id, poi)) — NOT a synthetic placeholder volume. Runs the real
# pipeline like worldd-itm1-chain-load: mcc emit-sql content/core -> throwaway MariaDB
# (world DDL + emitted DML) -> load_world_content -> drive AreaTriggerSet + QuestLog.
# Same TOOL-/ENV-guards (MERIDIAN_WORLDDB_*/MERIDIAN_DB_* + a mariadb/mysql client + an
# mcc binary); SKIPS (exit 0) otherwise — inert in the plain server ctest, real under
# test.sh --db / the DB CI job.
add_executable(worldd-explore-trigger-db-test
  test/explore_trigger_db_it.cpp
)
target_link_libraries(worldd-explore-trigger-db-test
  PRIVATE
    meridian::world-dispatch
)
target_compile_definitions(worldd-explore-trigger-db-test PRIVATE
  ITM1_CONTENT_DIR="${CMAKE_SOURCE_DIR}/../content"
  ITM1_WORLD_DDL_DIR="${CMAKE_SOURCE_DIR}/../schema/sql/world"
  ITM1_MCC_BIN="${CMAKE_BINARY_DIR}/../mcc/mcc")
set_target_properties(worldd-explore-trigger-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-explore-trigger-db COMMAND worldd-explore-trigger-db-test)

# --- IT-M1 10-quest CHAIN server-side HARNESS (#399; epic #20 EXIT CRITERION). --
# The capstone. Boots the DB-loaded IT-M1 content (mcc emit-sql content/core -> a
# throwaway MariaDB it owns, carrying world DDL + auth + characters schemas), stands
# up a REAL worldd session (WORLD_HELLO -> ENTER_WORLD over TLS), and drives the full
# 10-quest 'Emberfall Hollow' DAG to completion over the LIVE dispatch path — kill via
# MapTick + the quest-credit bus, collect via LOOT_TAKE, deliver via GOSSIP_HELLO,
# explore via a synthetic content-keyed TriggerVolume crossing. Asserts ZERO client-
# trusted outcomes (only intents in, server QUEST_PROGRESS/turn-in results out),
# server-side PREREQ GATING (convergence quest rejected before its prereqs), a valid
# topological completion (convergence quest last), and durable reward money. Same
# TOOL-/ENV-guards as worldd-itm1-chain-load (MERIDIAN_WORLDDB_*/MERIDIAN_DB_* + a
# mariadb/mysql client + an mcc binary); SKIPS (exit 0) otherwise — inert in the plain
# server ctest, real under test.sh --db / the DB CI job. Paths (content root, world
# DDL, auth + characters migrations, default mcc) are compile definitions.
add_executable(worldd-itm1-chain-run-test
  test/itm1_chain_run_it.cpp
)
target_link_libraries(worldd-itm1-chain-run-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    meridian::npc
    Threads::Threads
)
target_compile_definitions(worldd-itm1-chain-run-test PRIVATE
  ITM1_CONTENT_DIR="${CMAKE_SOURCE_DIR}/../content"
  ITM1_WORLD_DDL_DIR="${CMAKE_SOURCE_DIR}/../schema/sql/world"
  ITM1_AUTH_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/auth/migrations"
  ITM1_CHAR_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/characters/migrations"
  ITM1_MCC_BIN="${CMAKE_BINARY_DIR}/../mcc/mcc")
set_target_properties(worldd-itm1-chain-run-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-itm1-chain-run COMMAND worldd-itm1-chain-run-test)

# --- OPS-03c INSTRUMENTED CHEAT-CLIENT regression suite (#422, epic #21 EXIT). ---
# THE epic-#21 exit criterion: "speedhack/teleport/dupe attempts from the cheat client
# are all rejected and logged." Stands up a LIVE worldd session (real TLS listener +
# serve loop, throwaway MariaDB owning world+auth+characters + mcc content) like the
# #399 chain-run, then drives a battery of deliberate exploit attempts (speedhack /
# teleport / fabricated-flag movement, double-take loot dupe, impossible economy delta)
# and asserts EACH is REJECTED server-side (snap-back / typed error, no illegal gain)
# AND RECORDED on the OPS-05 anti-cheat/economy audit stream (captured from stdout).
# Same DB-gated skip contract as the chain-run: needs MERIDIAN_WORLDDB_*/MERIDIAN_DB_*,
# a mariadb/mysql client, and an mcc binary; SKIPS (exit 0) otherwise — inert in the
# plain server ctest, real under scripts/dev/test.sh --db / the DB CI job.
add_executable(worldd-cheat-client-suite-test
  test/cheat_client_suite_it.cpp
)
target_link_libraries(worldd-cheat-client-suite-test
  PRIVATE
    meridian::world-dispatch
    meridian::characters
    meridian::npc
    Threads::Threads
)
target_compile_definitions(worldd-cheat-client-suite-test PRIVATE
  CHEAT_CONTENT_DIR="${CMAKE_SOURCE_DIR}/../content"
  CHEAT_WORLD_DDL_DIR="${CMAKE_SOURCE_DIR}/../schema/sql/world"
  CHEAT_AUTH_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/auth/migrations"
  CHEAT_CHAR_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/characters/migrations"
  CHEAT_MCC_BIN="${CMAKE_BINARY_DIR}/../mcc/mcc")
set_target_properties(worldd-cheat-client-suite-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-cheat-client-suite COMMAND worldd-cheat-client-suite-test)

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

# --- Tier-1 ability-engine PRIMITIVE EXECUTION test (SP2.3; #693). -------------
# PURE (no DB, no socket): drives the ability engine (combat_resolver + AuraContainer
# + MapTick) with a FIXED CombatRng seed and asserts that EVERY effect primitive
# EXECUTES server-authoritatively + deterministically — dot/hot periodic ticks,
# buff/debuff attribute modifiers (StatKey + the #694 interim ledger), shield absorb,
# cc gating (stun/root/silence), resource grant/drain, movement displacement, summon
# spawn+despawn, and an ordered multi-primitive boss-slam chain. SP2.2 (#692) proved
# these LOAD; this proves they RUN. Always runs in the plain server ctest.
add_executable(worldd-ability-primitives-test
  test/ability_primitives_test.cpp
)
target_link_libraries(worldd-ability-primitives-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-ability-primitives-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-ability-primitives COMMAND worldd-ability-primitives-test)

# --- Effective-stat framework UNIT test (SP2.4; #694). ------------------------
# PURE (no DB, no socket): drives AttributeCatalog + EffectiveStats + AuraContainer
# directly — base + class/race attribute_mods + the live buff/debuff layer fold into
# one effective value (primary AND derived, flat AND percent), and effective() read
# straight off the SP2.3 aura ledger (attribute_delta) matches + rolls back on expiry.
# This is the framework's logic; the DB-backed proof (mods loaded from a real MariaDB)
# is the separate worldd-effective-stats-db test. Always runs in the plain server ctest.
add_executable(worldd-effective-stats-test
  test/effective_stats_test.cpp
)
target_link_libraries(worldd-effective-stats-test
  PRIVATE
    meridian::world-dispatch
)
set_target_properties(worldd-effective-stats-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME worldd-effective-stats COMMAND worldd-effective-stats-test)

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
