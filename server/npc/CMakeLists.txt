# SPDX-License-Identifier: Apache-2.0
#
# meridian-npc — the M1 NPC interaction system (NPC-01/02; issue #372). Server-
# authoritative gossip menus (options gated by quest state + vendor/trainer role
# flags) and trainers (learn an ability for copper, gated by class + level), with a
# read-only NPC datastore seam (placeholder set now, mcc #28 later — mirrors loot's
# LootTable seam and quest's QuestStore seam). Clean-room, Apache-2.0.
#
# Layering: the pure gossip/trainer domain is DB-free and worldd-free — it depends
# only on meridian::items (the ECO-01 int64 copper type + the transactional
# currency ops on `character.money`), exactly as meridian::loot does. The gossip
# planner reads the player's quest state through an abstract QuestStateView so it
# never depends on worldd's QuestLog (worldd, the consumer, adapts its log to the
# view). The "vendor" gossip option is a role FLAG on the NPC record — this library
# never links or calls into meridian::vendor.

add_library(meridian-npc-lib STATIC
  src/npc_def.cpp
  src/gossip.cpp
  src/trainer.cpp
)
add_library(meridian::npc ALIAS meridian-npc-lib)
target_include_directories(meridian-npc-lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(meridian-npc-lib PUBLIC meridian::items)
target_compile_features(meridian-npc-lib PUBLIC cxx_std_20)
set_target_properties(meridian-npc-lib PROPERTIES CXX_EXTENSIONS OFF)

# Deterministic, DB-FREE unit test: the pure npc domain — the placeholder NPC set's
# shape, the gossip planner (quest options reflect available/in-progress/complete
# state; vendor/trainer flags surface their options; deterministic order), and the
# trainer learn PLAN (teaches on valid class + level + copper; rejects on wrong
# class, too-low level, insufficient funds, already-known, non-trainer, unknown
# ability — money is a pure input, never touched on a rejection). Runs in the plain
# server ctest (needs no MariaDB), so it asserts real behaviour in every CI job.
add_executable(meridian-npc-test test/npc_test.cpp)
target_link_libraries(meridian-npc-test PRIVATE meridian::npc)
set_target_properties(meridian-npc-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME npc COMMAND meridian-npc-test)

# DB-backed integration test: drives learn_ability against a live characters DB and
# proves the copper actually debits on `character.money` (a valid learn debits the
# cost; every rejection leaves the balance untouched — all-or-nothing). Learned
# abilities are in-memory for M1 (no durable `character_ability` table yet — see
# trainer.h), so this asserts the copper debit through the real DB and the in-memory
# learned set, not a persisted ability row. Reads MERIDIAN_DB_* and SKIPS (exit 0)
# when none are set — inert in the DB-free ctest, real only in the DB CI job /
# `scripts/dev/test.sh --db`.
add_executable(meridian-npc-db-test test/trainer_db_it.cpp)
target_link_libraries(meridian-npc-db-test PRIVATE meridian::npc)
set_target_properties(meridian-npc-db-test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME npc-db COMMAND meridian-npc-db-test)
