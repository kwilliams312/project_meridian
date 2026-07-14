// SPDX-License-Identifier: Apache-2.0
//
// worldd — Creature mob AI implementation (issues #347 + #348, CMB-02).
//
// CLEAN-ROOM from docs/sad/server-sad.md §2.5 / §3.2 (see creature_ai.h header).
// No GPL / emulator source consulted (CONTRIBUTING.md). Every formula, radius and
// transition is derived from OUR SAD, not from any existing game's data or code.

#include "creature_ai.h"

#include <algorithm>
#include <cmath>

namespace meridian::worldd {

namespace {

// Planar (x,y) distance — Position.z is the vertical axis (movement_validation.h),
// so aggro / leash / arrival are measured on the horizontal ground plane.
float planar_dist(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// The outcome of one movement step toward a destination.
struct StepResult {
    Position pos;         // new position (facing set from the movement direction)
    bool arrived = false; // destination reached (snapped exactly onto it) this step
};

// Move `from` toward `to` by at most `max_dist` metres (a straight line — M1
// greybox linear waypoint following; navmesh splines are M3). If the destination
// is within reach it snaps exactly onto it and reports arrival. Facing
// (orientation) is the horizontal movement direction; when already on the point
// the prior facing is kept.
StepResult step_towards(const Position& from, const Position& to, float max_dist) {
    StepResult r;
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist <= kArriveEpsilonM || dist <= max_dist || max_dist <= 0.0f) {
        r.pos = to;
        r.arrived = dist <= max_dist || dist <= kArriveEpsilonM;
        // Face the way we travelled (keep prior facing if we did not actually move).
        r.pos.orientation = (dist > kArriveEpsilonM) ? std::atan2(dy, dx) : from.orientation;
        if (!r.arrived) {
            // max_dist <= 0 with a non-trivial gap: stand still, keep facing.
            r.pos = from;
        }
        return r;
    }

    const float t = max_dist / dist;
    r.pos.x = from.x + dx * t;
    r.pos.y = from.y + dy * t;
    r.pos.z = from.z + dz * t;
    r.pos.orientation = std::atan2(dy, dx);
    r.arrived = false;
    return r;
}

// Did the position actually change (worth an EntityUpdate)?
bool moved(const Position& a, const Position& b) {
    return a.x != b.x || a.y != b.y || a.z != b.z || a.orientation != b.orientation;
}

// Metres travelled this tick at `speed` m/s over `dt_ms`.
float step_budget(float speed, std::uint32_t dt_ms) {
    return speed * (static_cast<float>(dt_ms) / 1000.0f);
}

}  // namespace

// ---------------------------------------------------------------------------
// effective_aggro_radius — SAD §2.5 "aggro radius by level delta".
// ---------------------------------------------------------------------------
float effective_aggro_radius(float base_radius, int creature_level, int target_level) {
    const float eff =
        base_radius + static_cast<float>(creature_level - target_level) * kAggroRadiusPerLevel;
    const float lo = 0.0f;
    const float hi = base_radius + kAggroRadiusBonusCap;
    return std::clamp(eff, lo, hi);
}

// ---------------------------------------------------------------------------
// Spawns.
// ---------------------------------------------------------------------------
ObjectGuid CreatureAi::add_spawn(const CreatureSpawnDef& def) {
    const ObjectGuid guid = next_guid_++;
    const UnitStats stats = placeholder_creature_stats(def.level, def.faction);

    Instance inst{Creature(guid, def.home, stats, def.template_id), def};
    inst.unit.spawn();  // full health/resource, alive, at home
    inst.state = AiState::kPatrol;
    instances_.emplace(guid, std::move(inst));
    return guid;
}

bool CreatureAi::despawn(ObjectGuid guid) {
    return instances_.erase(guid) > 0;
}

std::vector<ObjectGuid> CreatureAi::load_placeholder_spawns(const Position& origin) {
    std::vector<ObjectGuid> out;

    auto at = [&](float dx, float dy) {
        Position p = origin;
        p.x += dx;
        p.y += dy;
        return p;
    };

    // A stationary sentinel a short way from origin.
    {
        CreatureSpawnDef d;
        d.template_id = 1001;
        d.level = 3;
        d.faction = Faction::kHostile;
        d.home = at(10.0f, 0.0f);
        d.aggro_base_radius = 8.0f;
        d.leash_radius = 25.0f;
        d.respawn_ms = 15000;
        d.move_speed = 4.0f;
        d.patrol_mode = PatrolMode::kStationary;
        out.push_back(add_spawn(d));
    }

    // A looping patroller walking a small square.
    {
        CreatureSpawnDef d;
        d.template_id = 1002;
        d.level = 4;
        d.faction = Faction::kHostile;
        d.home = at(-10.0f, 0.0f);
        d.aggro_base_radius = 10.0f;
        d.leash_radius = 40.0f;
        d.respawn_ms = 20000;
        d.move_speed = 3.5f;
        d.patrol_mode = PatrolMode::kLoop;
        d.waypoints = {at(-10.0f, 0.0f), at(-10.0f, 10.0f), at(-20.0f, 10.0f),
                       at(-20.0f, 0.0f)};
        out.push_back(add_spawn(d));
    }

    // A ping-pong patroller walking a straight line back and forth.
    {
        CreatureSpawnDef d;
        d.template_id = 1003;
        d.level = 2;
        d.faction = Faction::kHostile;
        d.home = at(0.0f, 20.0f);
        d.aggro_base_radius = 9.0f;
        d.leash_radius = 35.0f;
        d.respawn_ms = 18000;
        d.move_speed = 3.0f;
        d.patrol_mode = PatrolMode::kPingPong;
        d.waypoints = {at(0.0f, 20.0f), at(15.0f, 20.0f)};
        out.push_back(add_spawn(d));
    }

    return out;
}

// ---------------------------------------------------------------------------
// Threat input (from the combat resolver, #344).
// ---------------------------------------------------------------------------
void CreatureAi::add_threat(ObjectGuid creature_guid, ObjectGuid attacker_guid,
                            float amount) {
    if (amount <= 0.0f) return;
    auto it = instances_.find(creature_guid);
    if (it == instances_.end()) return;
    Instance& inst = it->second;

    // A dead creature accrues nothing; an evading (immune) creature has dropped its
    // threat and is not fighting — being outside its leash, it ignores new threat.
    if (inst.state == AiState::kDead || inst.state == AiState::kEvade) return;

    inst.threat[attacker_guid] += amount;

    // Threat pulls a resting creature into combat, targeting the attacker.
    if (inst.state == AiState::kPatrol) {
        inst.state = AiState::kCombat;
        inst.target = attacker_guid;
    }
}

// ---------------------------------------------------------------------------
// Tick.
// ---------------------------------------------------------------------------
CreatureAiTickResult CreatureAi::tick(std::uint32_t dt_ms,
                                      const std::vector<AiTargetView>& targets) {
    CreatureAiTickResult out;

    // Index targets by guid for O(1) threat-target lookup.
    std::unordered_map<ObjectGuid, const AiTargetView*> by_guid;
    by_guid.reserve(targets.size());
    for (const AiTargetView& t : targets) by_guid.emplace(t.guid, &t);

    // Deterministic iteration order (ascending guid) so tie-breaks + move ordering
    // are reproducible regardless of the hash map's internal order.
    std::vector<ObjectGuid> guids;
    guids.reserve(instances_.size());
    for (const auto& kv : instances_) guids.push_back(kv.first);
    std::sort(guids.begin(), guids.end());

    for (ObjectGuid guid : guids) {
        Instance& inst = instances_.at(guid);

        // Death edge: the resolver may have driven the Unit to 0 HP since last tick.
        if (inst.state != AiState::kDead && inst.unit.is_dead()) {
            inst.state = AiState::kDead;
            inst.respawn_remaining_ms = inst.def.respawn_ms;
            inst.threat.clear();
            inst.target = 0;
            inst.was_dead = true;
            out.despawned.push_back(guid);
            continue;  // no movement the tick it dies
        }

        switch (inst.state) {
            case AiState::kDead:
                tick_dead(inst, dt_ms, out);
                break;
            case AiState::kEvade:
                tick_evade(inst, dt_ms, out);
                break;
            case AiState::kCombat:
                tick_combat(inst, dt_ms, by_guid, out);
                break;
            case AiState::kPatrol:
                tick_patrol(inst, dt_ms, targets, out);
                break;
        }
    }

    return out;
}

void CreatureAi::tick_dead(Instance& inst, std::uint32_t dt_ms, CreatureAiTickResult& out) {
    if (inst.respawn_remaining_ms > dt_ms) {
        inst.respawn_remaining_ms -= dt_ms;
        return;
    }
    // Timer elapsed → respawn at home, full health, resting.
    inst.respawn_remaining_ms = 0;
    inst.unit.set_position(inst.def.home);
    inst.unit.spawn();  // full health/resource, alive
    inst.state = AiState::kPatrol;
    inst.target = 0;
    inst.threat.clear();
    inst.wp_index = 0;
    inst.wp_dir = 1;
    inst.was_dead = false;
    out.spawned.push_back(inst.unit.guid());
}

void CreatureAi::enter_evade(Instance& inst) {
    inst.state = AiState::kEvade;
    inst.target = 0;
    inst.threat.clear();
    // Full-heal happens on ARRIVAL home (tick_evade), so a chased-down creature is
    // healed only once it has actually leashed back.
}

void CreatureAi::tick_evade(Instance& inst, std::uint32_t dt_ms, CreatureAiTickResult& out) {
    const Position before = inst.unit.position();
    const float budget = step_budget(inst.def.move_speed, dt_ms);
    const StepResult s = step_towards(before, inst.def.home, budget);

    inst.unit.set_position(s.pos);
    if (moved(before, s.pos)) out.moves.push_back({inst.unit.guid(), s.pos});

    if (s.arrived) {
        // Home again: FULL-HEAL and resume patrolling from the route start.
        inst.unit.spawn();  // full health/resource
        inst.state = AiState::kPatrol;
        inst.wp_index = 0;
        inst.wp_dir = 1;
    }
}

ObjectGuid CreatureAi::select_threat_target(
    Instance& inst,
    const std::unordered_map<ObjectGuid, const AiTargetView*>& by_guid) const {
    // Prune threat for targets that have left the map or died — they no longer
    // hold aggro.
    for (auto it = inst.threat.begin(); it != inst.threat.end();) {
        auto tv = by_guid.find(it->first);
        if (tv == by_guid.end() || !tv->second->alive) {
            it = inst.threat.erase(it);
        } else {
            ++it;
        }
    }

    // Highest threat wins; ties broken by ascending guid (deterministic).
    ObjectGuid best = 0;
    float best_threat = -1.0f;
    for (const auto& kv : inst.threat) {
        if (kv.second > best_threat || (kv.second == best_threat && kv.first < best)) {
            best_threat = kv.second;
            best = kv.first;
        }
    }
    return best;
}

void CreatureAi::tick_combat(
    Instance& inst, std::uint32_t dt_ms,
    const std::unordered_map<ObjectGuid, const AiTargetView*>& by_guid,
    CreatureAiTickResult& out) {
    const ObjectGuid target = select_threat_target(inst, by_guid);
    if (target == 0) {
        // Nobody left to fight → leash home.
        enter_evade(inst);
        tick_evade(inst, dt_ms, out);
        return;
    }
    inst.target = target;

    // Leash: pulled too far from home → evade (drop threat, run home, heal).
    if (planar_dist(inst.unit.position(), inst.def.home) > inst.def.leash_radius) {
        enter_evade(inst);
        tick_evade(inst, dt_ms, out);
        return;
    }

    // Chase the target's current position (the resolver handles the actual attack
    // once in range; the AI only closes the distance).
    const AiTargetView* tv = by_guid.at(target);
    const Position before = inst.unit.position();
    const float budget = step_budget(inst.def.move_speed, dt_ms);
    const StepResult s = step_towards(before, tv->pos, budget);

    inst.unit.set_position(s.pos);
    if (moved(before, s.pos)) out.moves.push_back({inst.unit.guid(), s.pos});
}

void CreatureAi::tick_patrol(Instance& inst, std::uint32_t dt_ms,
                             const std::vector<AiTargetView>& targets,
                             CreatureAiTickResult& out) {
    // Proximity aggro: the nearest hostile, alive target inside the level-scaled
    // aggro radius pulls the creature into combat. Ties by ascending guid.
    ObjectGuid aggro = 0;
    float aggro_dist = 0.0f;
    for (const AiTargetView& t : targets) {
        if (!t.alive) continue;
        if (t.faction == inst.unit.faction()) continue;  // same side — not hostile
        const float radius =
            effective_aggro_radius(inst.def.aggro_base_radius, inst.unit.level(), t.level);
        if (radius <= 0.0f) continue;
        const float d = planar_dist(inst.unit.position(), t.pos);
        if (d > radius) continue;
        if (aggro == 0 || d < aggro_dist || (d == aggro_dist && t.guid < aggro)) {
            aggro = t.guid;
            aggro_dist = d;
        }
    }
    if (aggro != 0) {
        inst.threat[aggro] += kProximityAggroThreat;
        inst.state = AiState::kCombat;
        inst.target = aggro;
        return;  // begin chasing next tick
    }

    // No aggro → advance the patrol route (linear waypoint following, #348).
    if (inst.def.patrol_mode == PatrolMode::kStationary || inst.def.waypoints.size() < 2) {
        return;  // sentinel: hold position
    }

    const std::vector<Position>& wps = inst.def.waypoints;
    const Position before = inst.unit.position();
    const float budget = step_budget(inst.def.move_speed, dt_ms);
    const StepResult s = step_towards(before, wps[inst.wp_index], budget);

    inst.unit.set_position(s.pos);
    if (moved(before, s.pos)) out.moves.push_back({inst.unit.guid(), s.pos});

    if (s.arrived) {
        // Reached the current waypoint → pick the next by patrol mode.
        if (inst.def.patrol_mode == PatrolMode::kLoop) {
            inst.wp_index = (inst.wp_index + 1) % wps.size();
        } else {  // kPingPong
            if (inst.wp_index == wps.size() - 1) inst.wp_dir = -1;
            else if (inst.wp_index == 0) inst.wp_dir = 1;
            inst.wp_index = static_cast<std::size_t>(static_cast<int>(inst.wp_index) + inst.wp_dir);
        }
    }
}

// ---------------------------------------------------------------------------
// Introspection.
// ---------------------------------------------------------------------------
Creature* CreatureAi::creature(ObjectGuid guid) {
    auto it = instances_.find(guid);
    return it == instances_.end() ? nullptr : &it->second.unit;
}

const Creature* CreatureAi::creature(ObjectGuid guid) const {
    auto it = instances_.find(guid);
    return it == instances_.end() ? nullptr : &it->second.unit;
}

AiState CreatureAi::state_of(ObjectGuid guid) const {
    auto it = instances_.find(guid);
    return it == instances_.end() ? AiState::kDead : it->second.state;
}

ObjectGuid CreatureAi::target_of(ObjectGuid guid) const {
    auto it = instances_.find(guid);
    return it == instances_.end() ? 0 : it->second.target;
}

float CreatureAi::threat_of(ObjectGuid creature_guid, ObjectGuid attacker_guid) const {
    auto it = instances_.find(creature_guid);
    if (it == instances_.end()) return 0.0f;
    auto t = it->second.threat.find(attacker_guid);
    return t == it->second.threat.end() ? 0.0f : t->second;
}

ObjectGuid CreatureAi::top_threat(ObjectGuid creature_guid) const {
    auto it = instances_.find(creature_guid);
    if (it == instances_.end()) return 0;
    // Highest accrued threat wins; ties broken by ascending guid (deterministic) —
    // the same rule select_threat_target uses. This is the "damage attribution via
    // threat" a kill-XP award resolves the killer with when the killing blow has no
    // direct caster (a periodic/DoT tick — #360).
    ObjectGuid best = 0;
    float best_threat = -1.0f;
    for (const auto& kv : it->second.threat) {
        if (kv.second > best_threat || (kv.second == best_threat && kv.first < best)) {
            best_threat = kv.second;
            best = kv.first;
        }
    }
    return best;
}

}  // namespace meridian::worldd
