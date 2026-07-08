// SPDX-License-Identifier: Apache-2.0
//
// worldd — Creature mob AI (issues #347 + #348, CMB-02; part of epic #18).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md ONLY — §2.5 ("Entity / aggro /
// threat … Threat table per creature, aggro radius by level delta, leash-to-home
// with evade/full-heal, respawn timers, waypoint patrols from compiled spawn
// tables. Creature movement = Recast/Detour navmesh spline paths broadcast as
// spline packets") and §3.2 (the map tick order: "… AI update (threat, leash,
// waypoints, spline moves) … spawn/respawn timers …"). No GPL / AGPL / CMaNGOS /
// TrinityCore / leaked emulator source consulted — every formula, radius, and
// state transition here is ORIGINAL, derived from OUR SAD and the existing worldd
// model (combat_unit.h Creature, movement Position). See CONTRIBUTING.md.
//
// WHAT THIS FILE IS: the server-authoritative brain for a Creature (combat_unit.h),
// built as a SELF-CONTAINED module the single-threaded map tick calls in its AI
// phase. It owns each creature's simulation entity + AI runtime state and, per
// tick, produces the effects the tick applies to the world:
//
//   • THREAT (#347)   — a per-creature threat table (guid → accrued threat); the
//                        creature fights whoever holds the most threat.
//   • AGGRO  (#347)   — proximity aggro whose radius scales with the creature-vs-
//                        target LEVEL DELTA (a higher-level target is noticed from
//                        farther away shrinks — see effective_aggro_radius).
//   • LEASH  (#347)   — pulled too far from its spawn home the creature EVADES:
//                        drops threat, returns home, FULL-HEALS on arrival.
//   • RESPAWN(#347)   — a killed creature waits out a respawn timer, then respawns
//                        at home at full health.
//   • PATROL (#348)   — waypoint following (linear between points — M1 greybox
//                        scope; full Recast/Detour navmesh splines are M3 per the
//                        SAD) and per-tick creature MOVEMENT output shaped so the
//                        tick can hand it straight to the AoI relay
//                        (world_state.h encode_entity_update_payload takes exactly
//                        an entity guid + Position).
//
// OUT OF SCOPE (owned elsewhere, deliberately absent here): the combat resolver /
// GCD / attack tables (#344/#345 — this module NEVER rolls damage; it only decides
// WHO to fight and WHERE to stand, and receives threat from the resolver via
// add_threat), auras (#346), the ability-use handler + world.fbs wire schema, and
// the tick loop / golden scenarios (#349 — the tick CALLS this module; it is not
// built here). The AI holds NO wire types and NO locks: a map is single-threaded
// by construction (SAD §2.5/§6 — "the tick owns entity state"), so the tick
// serializes all access.
//
// DETERMINISM: tick() is a pure function of (dt, targets, prior state) — no wall
// clock, no RNG, no I/O. Tie-breaks are by ascending guid. This is what makes the
// aggro/threat/leash/respawn/patrol behavior seeded-deterministic unit-testable
// (the #347/#348 verification ask).

#ifndef MERIDIAN_WORLDD_CREATURE_AI_H
#define MERIDIAN_WORLDD_CREATURE_AI_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "combat_unit.h"          // Creature / Unit / Faction / ObjectGuid
#include "movement_validation.h"  // Position

namespace meridian::worldd {

// Guid base for AI-managed creatures. Kept clear of real (low-numbered) character
// guids AND of world_state.h's kSyntheticGuidBase (0xF000…) for placeholder
// players, so a creature and a player can never collide in the grid or on the
// wire. M1 placeholder allocation; at M2 spawns carry content-assigned guids.
inline constexpr ObjectGuid kCreatureGuidBase = 0xC000'0000'0000'0000ULL;

// How a creature traverses its waypoint list (#348).
enum class PatrolMode : std::uint8_t {
    kStationary = 0,  // no route — a sentinel that holds its spawn home
    kLoop = 1,        // … → w[n-1] → w[0] → w[1] → …  (wrap around)
    kPingPong = 2,    // … → w[n-1] → w[n-2] → …       (reverse at each end)
};

// The AI finite-state of one creature (its FSM position). Transitions are driven
// only by tick() (and add_threat, which can pull kPatrol → kCombat).
enum class AiState : std::uint8_t {
    kPatrol = 0,  // home/waypoints, scanning for aggro (the resting state)
    kCombat = 1,  // holds a live target: chase + keep the threat table
    kEvade = 2,   // leashed: threat dropped, returning home, full-heal on arrival
    kDead = 3,    // killed: counting down the respawn timer
};

// A data-driven spawn definition — the M1 placeholder for the compiled
// spawn_point / patrol_path world-DB rows (SAD §5.1 table families; real content
// via #28). ORIGINAL clean-room shape.
struct CreatureSpawnDef {
    std::uint32_t template_id = 0;
    std::uint16_t level = 1;
    Faction faction = Faction::kHostile;

    Position home;                     // spawn anchor AND the leash centre
    float aggro_base_radius = 0.0f;    // metres, at EQUAL level (level-delta scaled)
    float leash_radius = 0.0f;         // metres from home before the creature evades
    std::uint32_t respawn_ms = 0;      // delay from death until respawn
    float move_speed = 0.0f;           // metres/second (patrol + chase + return)

    PatrolMode patrol_mode = PatrolMode::kStationary;
    std::vector<Position> waypoints;   // patrol route (ignored when kStationary)
};

// One creature's per-tick movement result — shaped so the tick hands it straight
// to the AoI relay (world_state.h encode_entity_update_payload(guid, pos)). The
// Position carries the new facing in `orientation` (movement direction).
struct CreatureMove {
    ObjectGuid guid = 0;
    Position pos;
};

// The effect set the AI phase produces for ONE tick, for the tick to apply to the
// AoI relay:
//   • moves     — creatures whose position changed  → EntityUpdate.
//   • spawned   — creatures that (re)entered the world this tick → EntityEnter.
//   • despawned — creatures that DIED this tick → EntityLeave{DIED}.
// (A spawned creature is reported ONLY in `spawned`, not also in `moves`: the
// EntityEnter already carries its position.)
struct CreatureAiTickResult {
    std::vector<CreatureMove> moves;
    std::vector<ObjectGuid> spawned;
    std::vector<ObjectGuid> despawned;
};

// A read-only snapshot of a potential aggro/threat target (a Player or other
// hostile Unit) the tick supplies to the AI phase each tick. The AI never owns or
// mutates players — it only reads this view to decide aggro/chase.
struct AiTargetView {
    ObjectGuid guid = 0;
    Position pos;
    std::uint16_t level = 1;
    Faction faction = Faction::kPlayer;
    bool alive = true;
};

// Effective proximity-aggro radius given the creature-vs-target LEVEL DELTA
// (SAD §2.5 "aggro radius by level delta"). Pure + clean-room:
//
//     eff = base + (creature_level − target_level) · kAggroRadiusPerLevel
//
// clamped to [0, base + kAggroRadiusBonusCap]. So a target at the creature's own
// level aggros at `base`; a LOWER-level target is noticed from farther (a mob
// bullies the weak); a HIGHER-level target from nearer, and once far enough above
// the creature the radius hits 0 — the creature never proximity-aggros it at all
// (it "doesn't notice" something that outclasses it). Threat (add_threat) still
// pulls it into combat regardless of level — being hit is being hit.
float effective_aggro_radius(float base_radius, int creature_level, int target_level);

// Per-level metres the aggro radius grows (lower target) / shrinks (higher target).
inline constexpr float kAggroRadiusPerLevel = 1.0f;
// Cap on how far ABOVE `base` the level bonus can push the radius (a much lower
// target does not aggro from across the map).
inline constexpr float kAggroRadiusBonusCap = 5.0f;
// Planar distance (metres) at/under which a moving creature is "arrived".
inline constexpr float kArriveEpsilonM = 0.05f;
// Threat granted by proximity aggro, so the newly-aggroed target enters the threat
// table as the (initial) leader. Small — a single resolver hit outweighs it.
inline constexpr float kProximityAggroThreat = 1.0f;

// ---------------------------------------------------------------------------
// CreatureAi — the map's owner of every AI-managed creature + its brain.
// ---------------------------------------------------------------------------
//
// Owns each creature's Creature (by value) and AI runtime state. The tick creates
// one per map, seeds it with spawns, feeds resolver threat in via add_threat, and
// calls tick() once per map tick (in the AI phase). NOT thread-safe by design —
// the map that owns it is single-threaded (SAD §2.5/§6).
class CreatureAi {
public:
    CreatureAi() = default;

    CreatureAi(const CreatureAi&) = delete;
    CreatureAi& operator=(const CreatureAi&) = delete;

    // Spawn a creature from `def`. Returns its assigned guid. It starts kPatrol,
    // alive, at full health from placeholder_creature_stats(def.level, def.faction),
    // positioned at def.home. (The returned guid is what add_threat / the AoI relay
    // key on.)
    ObjectGuid add_spawn(const CreatureSpawnDef& def);

    // Seed a placeholder greybox spawn set around `origin` — the M1 stand-in until
    // the content pipeline (#28) compiles real spawn_point/patrol_path rows. A mix
    // of a stationary sentinel and a looping/ping-pong patroller. Returns the guids
    // added (in spawn order).
    std::vector<ObjectGuid> load_placeholder_spawns(const Position& origin);

    // Resolver threat input (#344): `attacker_guid` dealt `amount` threat to the
    // creature `creature_guid`. Adds to the threat table and, if the creature is
    // patrolling, pulls it into combat targeting the attacker. No-op if the guid is
    // unknown or the creature is dead/evading (an evading creature is immune and
    // holds no threat). `amount` <= 0 is ignored.
    void add_threat(ObjectGuid creature_guid, ObjectGuid attacker_guid, float amount);

    // The AI phase of ONE map tick (SAD §3.2). Advances every creature by `dt_ms`
    // and returns the frame's effects (see CreatureAiTickResult). `targets` is the
    // tick's snapshot of aggroable Units (players) on the map. Deterministic.
    CreatureAiTickResult tick(std::uint32_t dt_ms, const std::vector<AiTargetView>& targets);

    // --- Introspection for the tick + tests (no wire coupling) ----------------
    std::size_t size() const { return instances_.size(); }
    // The creature's simulation entity (health/level/position…), or nullptr if the
    // guid is unknown. Stable across the creature's lifetime (map keeps addresses).
    // The non-const overload is the resolver seam — the tick hands the owning
    // (single-threaded) map the Creature so the combat resolver (#344) can
    // apply_damage / kill it, mirroring world_state.h's unit_for_slot. It is NOT a
    // handle for another thread to race on.
    Creature* creature(ObjectGuid guid);
    const Creature* creature(ObjectGuid guid) const;
    AiState state_of(ObjectGuid guid) const;         // kDead for an unknown guid
    ObjectGuid target_of(ObjectGuid guid) const;     // 0 = no current target
    float threat_of(ObjectGuid creature_guid, ObjectGuid attacker_guid) const;  // 0 if none
    // The attacker holding the most threat on `creature_guid` (0 if none / unknown).
    // Ties break by ascending guid. Used for kill-XP attribution (#360) when the
    // killing blow has no direct caster (a periodic tick).
    ObjectGuid top_threat(ObjectGuid creature_guid) const;

private:
    struct Instance {
        Creature unit;                                  // owned simulation entity
        CreatureSpawnDef def;
        AiState state = AiState::kPatrol;
        std::unordered_map<ObjectGuid, float> threat{};  // attacker guid → threat
        ObjectGuid target = 0;                          // current chase/threat target
        std::uint32_t respawn_remaining_ms = 0;         // kDead countdown
        std::size_t wp_index = 0;                       // waypoint being headed TO
        int wp_dir = 1;                                 // +1/-1 (ping-pong direction)
        bool was_dead = false;                          // death-edge detector
    };

    // Per-state tick bodies. Each may append to `out` and mutates `inst`.
    void tick_dead(Instance& inst, std::uint32_t dt_ms, CreatureAiTickResult& out);
    void tick_evade(Instance& inst, std::uint32_t dt_ms, CreatureAiTickResult& out);
    void tick_combat(Instance& inst, std::uint32_t dt_ms,
                     const std::unordered_map<ObjectGuid, const AiTargetView*>& by_guid,
                     CreatureAiTickResult& out);
    void tick_patrol(Instance& inst, std::uint32_t dt_ms,
                     const std::vector<AiTargetView>& targets, CreatureAiTickResult& out);

    // Enter the evade state: drop threat + target, begin the run home. Full-heal
    // happens on ARRIVAL (in tick_evade), not here.
    void enter_evade(Instance& inst);
    // Pick the highest-threat target still present + alive in `by_guid`, pruning
    // stale threat entries. Returns 0 if the threat table is now empty.
    ObjectGuid select_threat_target(
        Instance& inst,
        const std::unordered_map<ObjectGuid, const AiTargetView*>& by_guid) const;

    std::unordered_map<ObjectGuid, Instance> instances_;
    ObjectGuid next_guid_ = kCreatureGuidBase;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_CREATURE_AI_H
