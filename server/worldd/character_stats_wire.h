// SPDX-License-Identifier: Apache-2.0
//
// worldd — the CHARACTER_STATS wire encoder (SP2.5 #897; epic #866 S5b; split from
// #871 per the 2026-07-17 scout).
//
// WHAT THIS FILE IS: the single, thread-agnostic encoder that turns the #896
// aggregator's AggregatedCharacterStats snapshot into the CHARACTER_STATS (0x0022)
// FlatBuffer root-table bytes the OWNING client's stats panel decodes. It is the wire
// half of S5b; the push half (resolving the equipped loadout + routing the frame down
// the owning session's egress on enter-world / equip-change / level-up) lives in
// world_dispatch.cpp so it can reach the ConnCtx + the AttributeCatalog.
//
// Kept in its OWN small file (not file-local in world_dispatch.cpp like
// encode_inventory_snapshot) so it can be unit-tested in the PLAIN server ctest with
// NO DB and NO socket — an encode-then-decode round-trip over a hand-built snapshot,
// mirroring how the #896 aggregator itself is unit-tested.
//
// ⛔ SECURITY FENCE: this encodes the OWNER-PRIVATE sheet. It is NEVER the encoder for
// the AoI-broadcast EntityEnter/EntityUpdate `attrs` vectors (world_state.cpp) — those
// stay hardcoded empty on purpose, because a stat sheet on the AoI broadcast leaks
// every player's build to every observer (ratified decision). The dispatcher pushes
// these bytes through send_s2c → the owning session's egress ONLY.
//
// CLEAN-ROOM: composed from the #896 aggregator output model, the world.fbs
// CharacterStats contract, and the flatc-generated codec only. No GPL/AGPL/emulator
// source consulted (CONTRIBUTING.md).

#ifndef MERIDIAN_WORLDD_CHARACTER_STATS_WIRE_H
#define MERIDIAN_WORLDD_CHARACTER_STATS_WIRE_H

#include <cstdint>
#include <vector>

#include "effective_stats_aggregator.h"  // AggregatedCharacterStats

namespace meridian::worldd {

// Encode an aggregated effective-stat snapshot as the CHARACTER_STATS (0x0022)
// FlatBuffer ROOT-TABLE bytes (no transport framing — the caller adds the opcode/seq
// header via encode_frame). Every attribute in the snapshot becomes one
// CharacterStatEntry (ref + effective value); the snapshot's level + gear_armor ride
// the root table. Pure — no DB/socket/thread; safe to call from any thread.
std::vector<std::uint8_t> encode_character_stats(const AggregatedCharacterStats& stats);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_CHARACTER_STATS_WIRE_H
