// SPDX-License-Identifier: Apache-2.0
//
// worldd — player death state machine + corpse lifecycle (issue #359, CMB-03;
// part of epic #19). The server-authoritative death→resurrect flow the single
// "unit died" hook (map_tick.cpp) drives for a PLAYER:
//
//   kAlive ── dies ──► kCorpse ── release ──► kGhost ── corpse-run rez ──► kAlive
//                      (corpse spawned,       (at graveyard,   (restore health %,
//                       release timer)         running home)    despawn corpse)
//
//   • ON DEATH        — spawn a Corpse (combat_unit.h) at the death spot and arm
//                        the graveyard-release countdown (kCorpse).
//   • RELEASE         — auto (timer elapsed) OR requested (C→S) → become a ghost
//                        teleported to the nearest graveyard (kGhost).
//   • CORPSE-RUN      — the ghost runs back to its corpse; resurrection is allowed
//                        only once it is standing at the corpse.
//   • RESURRECT       — restore a % of max health, clear the ghost, despawn the
//                        corpse → alive again.
//
// CLEAN-ROOM: designed from docs/prd/server-prd.md (CMB-03: "death state machine,
// corpse object, release-to-graveyard (nearest graveyard from world data),
// corpse-run resurrect, resurrection sickness knobs from content data") and
// docs/sad/server-sad.md §2.5 (the shallow WorldObject→…, Corpse hierarchy + the
// spawn/respawn tick phase) ONLY. Every state, timer, and radius here is ORIGINAL,
// derived from OUR PRD/SAD + the existing worldd model. No GPL / AGPL / CMaNGOS /
// TrinityCore / leaked emulator source consulted. See CONTRIBUTING.md.
//
// M1 PLACEHOLDER: the graveyard is supplied by the caller (a fixed per-map point
// for M1); the "nearest graveyard from world data" lookup + resurrection-sickness
// knobs load from the world DB via content epic #28 — this module is the seam.
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE: a plain deterministic FSM over
// in-memory records. Time is passed IN as dt_ms (like creature_ai / aura_container),
// so it is unit-testable in the plain `server` ctest with no MariaDB. It owns the
// Corpse objects + death records but no locks — the single-threaded map tick that
// owns it serializes all access (SAD §2.5/§6).

#ifndef MERIDIAN_WORLDD_DEATH_STATE_H
#define MERIDIAN_WORLDD_DEATH_STATE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "combat_unit.h"          // Corpse / ObjectGuid
#include "movement_validation.h"  // Position

namespace meridian::worldd {

// Guid base for corpse objects. Kept clear of real character guids, the AI's
// kCreatureGuidBase (0xC000…), and world_state.h's kSyntheticGuidBase (0xF000…),
// so a corpse can never collide with a live entity on the grid or the wire.
// M1 placeholder allocation; content-assigned corpse guids land later.
inline constexpr ObjectGuid kCorpseGuidBase = 0xD000'0000'0000'0000ULL;

// Death-flow tunables (CMB-03). M1 placeholder values; the real values
// (per-graveyard timers, resurrection-sickness) load from world data (#28).
struct DeathConfig {
    std::uint32_t auto_release_ms = 6000;     // countdown until auto graveyard release
    std::uint32_t resurrect_health_pct = 50;  // % of max health restored on resurrect
    float corpse_run_radius_m = 2.0f;         // must be within this of the corpse to rez
};

// The death-flow phase of one player.
enum class DeathPhase : std::uint8_t {
    kAlive = 0,   // not dead (no record)
    kCorpse = 1,  // just died: corpse spawned, release countdown running
    kGhost = 2,   // released to the graveyard, running back to the corpse
};

const char* death_phase_name(DeathPhase p);

// One dead player's death-flow state.
struct DeathRecord {
    DeathPhase    phase = DeathPhase::kCorpse;
    ObjectGuid    corpse_guid = 0;
    Position      corpse_pos;                 // death spot = corpse-run destination
    Position      graveyard_pos;              // release destination (world-data seam)
    std::uint32_t release_remaining_ms = 0;   // auto-release countdown (kCorpse only)
};

// Why a resurrection attempt was refused (mirrors world.fbs ResurrectStatus).
enum class ResurrectReject : std::uint8_t {
    kNone = 0,       // OK
    kNotDead,        // the player is not dead
    kNotReleased,    // still kCorpse — release to the graveyard first
    kTooFar,         // kGhost but corpse-run not complete
};

// ---------------------------------------------------------------------------
// DeathStateMachine — the map's owner of every dead player's death flow + corpse.
// ---------------------------------------------------------------------------
class DeathStateMachine {
public:
    explicit DeathStateMachine(DeathConfig cfg = {}) : cfg_(cfg) {}

    DeathStateMachine(const DeathStateMachine&) = delete;
    DeathStateMachine& operator=(const DeathStateMachine&) = delete;

    const DeathConfig& config() const { return cfg_; }

    // A player died at `death_pos`; `graveyard_pos` is where a release sends the
    // ghost. Spawns a Corpse at the death spot (returns its guid), enters kCorpse
    // with the release timer armed. Replaces any prior record (a fresh death).
    ObjectGuid on_death(ObjectGuid player_guid, const Position& death_pos,
                        const Position& graveyard_pos);

    // Request an early release (C→S). kCorpse → kGhost immediately. Returns true if
    // it applied (the player was kCorpse).
    bool request_release(ObjectGuid player_guid);

    // Advance auto-release timers by `dt_ms`. Any kCorpse whose timer elapses this
    // step transitions to kGhost and is appended to `auto_released`. Deterministic.
    void advance(std::uint32_t dt_ms, std::vector<ObjectGuid>& auto_released);

    // Whether `player_guid` may resurrect from `at_pos` right now — kGhost AND
    // within corpse_run_radius_m of its corpse (corpse-run complete). Returns the
    // specific reject reason via `why` (kNone when allowed).
    bool can_resurrect(ObjectGuid player_guid, const Position& at_pos,
                       ResurrectReject& why) const;

    // Complete resurrection: erase the record + corpse and return the corpse guid
    // that must be despawned (0 if the guid had no record). Caller restores the
    // Unit's health (see resurrect_health). Does NOT re-check can_resurrect —
    // call that first.
    ObjectGuid resurrect(ObjectGuid player_guid);

    // Health to restore on resurrect for a given `max_health` (config %, clamped to
    // [1, max_health]).
    std::uint32_t resurrect_health(std::uint32_t max_health) const;

    // --- queries -----------------------------------------------------------
    DeathPhase phase_of(ObjectGuid player_guid) const;  // kAlive if no record
    const DeathRecord* record(ObjectGuid player_guid) const;
    const Corpse* corpse(ObjectGuid corpse_guid) const;
    std::size_t dead_count() const { return records_.size(); }

private:
    DeathConfig cfg_;
    std::unordered_map<ObjectGuid, DeathRecord> records_;   // player guid → record
    std::unordered_map<ObjectGuid, Corpse> corpses_;        // corpse guid → object
    ObjectGuid next_corpse_guid_ = kCorpseGuidBase;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_DEATH_STATE_H
