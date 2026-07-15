// SPDX-License-Identifier: Apache-2.0
//
// worldd — Creature mob AI UNIT TEST (issues #347 + #348, CMB-02).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5/§3.2 and creature_ai.h /
// combat_unit.h only. No GPL / emulator source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE / DETERMINISTIC: tick() is a pure function of
// (dt, targets, prior state) — no wall clock, no RNG — so every scenario below is
// reproducible. Runs in the plain server ctest.
//
// What it proves:
//   A. AGGRO BY LEVEL DELTA — effective_aggro_radius grows for lower-level targets,
//      shrinks for higher, and hits 0 when the target far outranks the creature;
//      proximity aggro obeys it and picks the NEAREST hostile (tie by guid).
//   B. THREAT ORDERING — the creature fights the highest-threat attacker; adding
//      threat re-targets; ties break by ascending guid; add_threat pulls a resting
//      creature into combat.
//   C. LEASH → EVADE → FULL-HEAL — chased past its leash the creature drops threat,
//      returns home, and is at full health on arrival.
//   D. RESPAWN TIMING — a killed creature despawns, waits out exactly its respawn
//      window, then respawns at home at full health.
//   E. WAYPOINT PATROL + MOVEMENT OUTPUT — loop + ping-pong routes progress through
//      their waypoints and emit CreatureMove (guid + advanced Position) per tick.

#include "combat_unit.h"
#include "creature_ai.h"
#include "movement_constants.h"
#include "movement_validation.h"  // Position

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

Position at(float x, float y) {
    Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

bool near_eq(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// A hostile-to-creatures player target snapshot.
AiTargetView player(ObjectGuid guid, Position pos, std::uint16_t level = 5) {
    AiTargetView v;
    v.guid = guid;
    v.pos = pos;
    v.level = level;
    v.faction = Faction::kPlayer;
    v.alive = true;
    return v;
}

// A stationary (non-moving, non-patrolling) creature so a test can isolate threat /
// aggro logic without chase movement or leash interfering.
CreatureSpawnDef sentinel(Position home, std::uint16_t level, float aggro, float leash) {
    CreatureSpawnDef d;
    d.template_id = 1;
    d.level = level;
    d.faction = Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = aggro;
    d.leash_radius = leash;
    d.respawn_ms = 1000;
    d.move_speed = 0.0f;  // stays put — no chase movement
    d.patrol_mode = PatrolMode::kStationary;
    return d;
}

// ---------------------------------------------------------------------------
void test_aggro_by_level_delta() {
    std::printf("A. aggro radius by level delta\n");

    // Pure formula: base 10, creature level 5.
    check("equal level -> base radius", near_eq(effective_aggro_radius(10.0f, 5, 5), 10.0f));
    check("lower-level target -> larger radius",
          near_eq(effective_aggro_radius(10.0f, 5, 3), 12.0f));
    check("higher-level target -> smaller radius",
          near_eq(effective_aggro_radius(10.0f, 5, 8), 7.0f));
    check("far-higher target -> zero radius (never proximity-aggros)",
          near_eq(effective_aggro_radius(10.0f, 5, 20), 0.0f));
    check("far-lower target -> radius capped at base + bonus cap",
          near_eq(effective_aggro_radius(10.0f, 20, 1), 15.0f));  // cap = base(10)+5

    // Behavior: a same-level target just inside the base radius aggros.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), /*level=*/5, /*aggro=*/10, /*leash=*/100));
        ai.tick(100, {player(42, at(9, 0), /*level=*/5)});
        check("same-level target inside base radius aggros", ai.state_of(c) == AiState::kCombat);
        check("aggro sets the target", ai.target_of(c) == 42);
    }
    // A higher-level target at distance 9 is OUTSIDE the shrunk radius (7): no aggro.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 10, 100));
        ai.tick(100, {player(42, at(9, 0), /*level=*/8)});
        check("higher-level target beyond shrunk radius does NOT aggro",
              ai.state_of(c) == AiState::kPatrol);
    }
    // Same higher-level target, now inside the shrunk radius (distance 6 < 7): aggro.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 10, 100));
        ai.tick(100, {player(42, at(6, 0), /*level=*/8)});
        check("higher-level target inside shrunk radius aggros", ai.state_of(c) == AiState::kCombat);
    }
    // A vastly higher-level target adjacent (radius 0): still no aggro.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 10, 100));
        ai.tick(100, {player(42, at(1, 0), /*level=*/30)});
        check("far-higher target adjacent does NOT aggro (radius 0)",
              ai.state_of(c) == AiState::kPatrol);
    }
    // Nearest hostile is chosen; tie broken by ascending guid.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 10, 100));
        ai.tick(100, {player(20, at(8, 0)), player(10, at(3, 0))});
        check("nearest target aggros first", ai.target_of(c) == 10);
    }
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 10, 100));
        // Both at equal distance 5 -> lower guid wins.
        ai.tick(100, {player(20, at(5, 0)), player(10, at(0, 5))});
        check("equal-distance tie breaks to lower guid", ai.target_of(c) == 10);
    }
    // A same-faction (friendly) unit never aggros.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 10, 100));
        AiTargetView friendly = player(42, at(2, 0));
        friendly.faction = Faction::kHostile;  // same side as the creature
        ai.tick(100, {friendly});
        check("same-faction unit does NOT aggro", ai.state_of(c) == AiState::kPatrol);
    }
}

// ---------------------------------------------------------------------------
void test_threat_ordering() {
    std::printf("B. threat table ordering + target selection\n");

    // add_threat pulls a resting creature straight into combat on the attacker.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 0, 100));  // no proximity aggro
        check("starts patrolling", ai.state_of(c) == AiState::kPatrol);
        ai.add_threat(c, /*attacker=*/7, 3.0f);
        check("threat pulls into combat", ai.state_of(c) == AiState::kCombat);
        check("targets the attacker", ai.target_of(c) == 7);
        check("threat recorded", near_eq(ai.threat_of(c, 7), 3.0f));
    }

    // Highest threat is fought; a bigger hit re-targets. Targets present + alive.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 0, 100));
        ai.add_threat(c, 1, 5.0f);
        ai.add_threat(c, 2, 10.0f);
        std::vector<AiTargetView> tv = {player(1, at(1, 0)), player(2, at(2, 0))};
        ai.tick(100, tv);
        check("fights the higher-threat attacker", ai.target_of(c) == 2);
        ai.add_threat(c, 1, 10.0f);  // attacker 1 now leads 15 vs 10
        ai.tick(100, tv);
        check("re-targets when threat lead changes", ai.target_of(c) == 1);
    }

    // Equal threat -> lowest guid wins (deterministic tie-break).
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 0, 100));
        ai.add_threat(c, 2, 8.0f);
        ai.add_threat(c, 1, 8.0f);
        ai.tick(100, {player(1, at(1, 0)), player(2, at(2, 0))});
        check("equal-threat tie breaks to lower guid", ai.target_of(c) == 1);
    }

    // A target that dies / leaves drops out of threat; last one gone -> evade.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(sentinel(at(0, 0), 5, 0, 100));
        ai.add_threat(c, 1, 5.0f);
        AiTargetView dead = player(1, at(1, 0));
        dead.alive = false;
        ai.tick(100, {dead});
        check("dead attacker drops threat", near_eq(ai.threat_of(c, 1), 0.0f));
        check("no valid target -> leashes to evade/patrol", ai.state_of(c) != AiState::kCombat);
    }
}

// ---------------------------------------------------------------------------
void test_basic_attack_intents() {
    std::printf("C. authored range/cadence + friendly safety\n");

    CreatureSpawnDef d = sentinel(at(0, 0), 1, /*aggro=*/10, /*leash=*/30);
    d.damage_min = 3;
    d.damage_max = 5;
    d.attack_speed_ms = 1000;
    d.move_speed = 2.0f;

    // Aggro transitions on one tick; combat then chases to exactly melee range
    // and swings immediately, never moving through the target.
    {
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(d);
        CreatureAiTickResult r = ai.tick(100, {player(42, at(10, 0), 1)});
        check("aggro transition emits no premature attack", r.attacks.empty());
        r = ai.tick(1000, {player(42, at(10, 0), 1)});
        check("first chase step remains out of range", r.attacks.empty());
        r = ai.tick(1000, {player(42, at(10, 0), 1)});
        check("second chase step remains out of range", r.attacks.empty());
        r = ai.tick(1000, {player(42, at(10, 0), 1)});
        check("chase stops at 5m and emits first swing",
              near_eq(ai.creature(c)->position().x, 5.0f) && r.attacks.size() == 1 &&
                  r.attacks[0].attacker_guid == c && r.attacks[0].target_guid == 42 &&
                  r.attacks[0].damage_min == 3 && r.attacks[0].damage_max == 5);

        r = ai.tick(999, {player(42, at(10, 0), 1)});
        check("cadence blocks early repeat", r.attacks.empty());
        r = ai.tick(1, {player(42, at(10, 0), 1)});
        check("cadence fires exactly at authored interval", r.attacks.size() == 1);

        AiTargetView dead = player(42, at(10, 0), 1);
        dead.alive = false;
        r = ai.tick(1000, {dead});
        check("target death stops attacks immediately", r.attacks.empty());
    }

    // Leashing clears the old cadence. After returning home and reacquiring, the
    // mob is entitled to a fresh immediate swing rather than carrying stale time.
    {
        CreatureSpawnDef leasher = d;
        leasher.leash_radius = 6.0f;
        leasher.move_speed = 10.0f;
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(leasher);
        ai.tick(100, {player(42, at(1, 0), 1)});  // acquire
        CreatureAiTickResult r = ai.tick(100, {player(42, at(1, 0), 1)});
        check("pre-leash combat swings immediately", r.attacks.size() == 1);

        r = ai.tick(1000, {player(42, at(20, 0), 1)});  // chase beyond leash
        check("chase toward runaway emits no out-of-range attack", r.attacks.empty());
        r = ai.tick(1000, {player(42, at(20, 0), 1)});  // evade + return home
        check("leash reset emits no attack and returns to patrol",
              r.attacks.empty() && ai.state_of(c) == AiState::kPatrol);
        ai.tick(100, {player(42, at(1, 0), 1)});  // reacquire
        r = ai.tick(100, {player(42, at(1, 0), 1)});
        check("post-leash reacquire restarts with an immediate swing",
              r.attacks.size() == 1);
    }

    // Death and respawn likewise clear attack state before a fresh acquisition.
    {
        CreatureSpawnDef respawner = d;
        respawner.respawn_ms = 100;
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(respawner);
        ai.tick(100, {player(42, at(1, 0), 1)});
        check("pre-death combat emits a swing",
              ai.tick(100, {player(42, at(1, 0), 1)}).attacks.size() == 1);
        ai.creature(c)->kill();
        CreatureAiTickResult r = ai.tick(100, {player(42, at(1, 0), 1)});
        check("death edge emits no attack", r.attacks.empty());
        r = ai.tick(100, {player(42, at(1, 0), 1)});
        check("respawn emits no attack", r.attacks.empty() && r.spawned.size() == 1);
        ai.tick(100, {player(42, at(1, 0), 1)});
        r = ai.tick(100, {player(42, at(1, 0), 1)});
        check("post-respawn reacquire restarts with an immediate swing",
              r.attacks.size() == 1);
    }

    // Required stats on a friendly passive NPC cannot make it attack.
    {
        CreatureSpawnDef friendly = d;
        friendly.faction = Faction::kFriendly;
        friendly.behavior = CreatureBehavior::kPassive;
        CreatureAi ai;
        const ObjectGuid c = ai.add_spawn(friendly);
        CreatureAiTickResult r = ai.tick(5000, {player(42, at(1, 0), 1)});
        ai.add_threat(c, 42, 100.0f);
        CreatureAiTickResult r2 = ai.tick(5000, {player(42, at(1, 0), 1)});
        check("friendly passive NPC never aggros or attacks",
              ai.state_of(c) == AiState::kPatrol && r.attacks.empty() &&
                  r2.attacks.empty() && ai.threat_of(c, 42) == 0.0f);
    }
}

// ---------------------------------------------------------------------------
void test_leash_evade_heal() {
    std::printf("D. leash -> evade -> full-heal\n");

    CreatureSpawnDef d;
    d.template_id = 1;
    d.level = 5;
    d.faction = Faction::kHostile;
    d.home = at(0, 0);
    d.aggro_base_radius = 0.0f;  // isolate: pulled in by threat only
    d.leash_radius = 20.0f;
    d.respawn_ms = 1000;
    d.move_speed = 30.0f;  // 30 m/tick at dt=1000
    d.patrol_mode = PatrolMode::kStationary;

    CreatureAi ai;
    const ObjectGuid c = ai.add_spawn(d);
    const std::uint32_t max_hp = ai.creature(c)->max_health();

    // Wound the creature and pull it into combat on a target far past the leash.
    ai.creature(c)->apply_damage(max_hp / 2);
    check("creature wounded below max", ai.creature(c)->health() < max_hp);
    ai.add_threat(c, /*attacker=*/9, 5.0f);
    const std::vector<AiTargetView> tv = {player(9, at(100, 0))};

    // Tick 1: in-leash, chases toward the runaway (0,0) -> (30,0).
    ai.tick(1000, tv);
    check("chases the target while in leash", ai.state_of(c) == AiState::kCombat);
    check("moved toward the target", near_eq(ai.creature(c)->position().x, 30.0f));

    // Tick 2: now 30 m from home > leash 20 -> evade (threat dropped) + step home.
    ai.tick(1000, tv);
    check("past leash -> evading", ai.state_of(c) == AiState::kEvade);
    check("threat dropped on evade", near_eq(ai.threat_of(c, 9), 0.0f));
    check("still wounded mid-return", ai.creature(c)->health() < max_hp);

    // Tick 3: reaches home (30 m back at 30 m/tick) -> full-heal + resume patrol.
    ai.tick(1000, tv);
    check("returned home", near_eq(ai.creature(c)->position().x, 0.0f));
    check("full-healed on arrival", ai.creature(c)->health() == max_hp);
    check("resumes patrol after evade", ai.state_of(c) == AiState::kPatrol);
}

// ---------------------------------------------------------------------------
void test_respawn_timing() {
    std::printf("E. respawn timing\n");

    CreatureSpawnDef d = sentinel(at(5, 5), /*level=*/4, /*aggro=*/0, /*leash=*/100);
    d.respawn_ms = 1000;

    CreatureAi ai;
    const ObjectGuid c = ai.add_spawn(d);
    const std::uint32_t max_hp = ai.creature(c)->max_health();

    // Kill it (as the resolver would) and tick: death is detected -> despawn, and
    // the respawn window (1000 ms) starts counting from the NEXT tick.
    ai.creature(c)->kill();
    check("creature is dead pre-tick", ai.creature(c)->is_dead());
    CreatureAiTickResult r = ai.tick(400, {});
    check("death reported as despawn", r.despawned.size() == 1 && r.despawned[0] == c);
    check("state is dead", ai.state_of(c) == AiState::kDead);

    // 500 ms of a 1000 ms window elapsed -> still dead, no respawn yet.
    r = ai.tick(500, {});
    check("still dead before timer elapses", ai.state_of(c) == AiState::kDead);
    check("no spawn yet", r.spawned.empty());

    // The remaining 500 ms elapses (500 + 500 >= 1000) -> respawn at home, full
    // health, patrolling.
    r = ai.tick(500, {});
    check("respawn reported", r.spawned.size() == 1 && r.spawned[0] == c);
    check("respawned alive at full health", ai.creature(c)->is_alive() &&
                                                ai.creature(c)->health() == max_hp);
    check("respawns at home", near_eq(ai.creature(c)->position().x, 5.0f) &&
                                  near_eq(ai.creature(c)->position().y, 5.0f));
    check("respawns patrolling", ai.state_of(c) == AiState::kPatrol);
}

// ---------------------------------------------------------------------------
void test_waypoint_patrol() {
    std::printf("F. waypoint patrol progression + movement output\n");

    // Loop patrol: home == w0 (0,0) -> w1 (0,10) -> w2 (10,10) -> w3 (10,0) -> wrap.
    CreatureSpawnDef d;
    d.template_id = 1;
    d.level = 3;
    d.faction = Faction::kHostile;
    d.home = at(0, 0);
    d.aggro_base_radius = 0.0f;  // no players present anyway
    d.leash_radius = 100.0f;
    d.respawn_ms = 1000;
    d.move_speed = 5.0f;  // 5 m/tick at dt=1000
    d.patrol_mode = PatrolMode::kLoop;
    d.waypoints = {at(0, 0), at(0, 10), at(10, 10), at(10, 0)};

    CreatureAi ai;
    const ObjectGuid c = ai.add_spawn(d);

    // Tick 1: already at w0 -> advances toward w1, no net move this tick.
    ai.tick(1000, {});
    check("starts at home", near_eq(ai.creature(c)->position().y, 0.0f));

    // Tick 2: heads to w1 (0,10), 5 m of the 10 m leg -> (0,5), facing +y.
    CreatureAiTickResult r = ai.tick(1000, {});
    check("emits a CreatureMove while patrolling", r.moves.size() == 1 && r.moves[0].guid == c);
    check("advanced 5 m toward w1", near_eq(ai.creature(c)->position().y, 5.0f));
    check("faces the travel direction (+y)",
          near_eq(ai.creature(c)->position().orientation, static_cast<float>(M_PI) / 2.0f));

    // Tick 3: reaches w1 (0,10).
    ai.tick(1000, {});
    check("reaches w1", near_eq(ai.creature(c)->position().y, 10.0f) &&
                            near_eq(ai.creature(c)->position().x, 0.0f));

    // Ticks 4-5: legs toward w2 (10,10): (5,10) then (10,10).
    ai.tick(1000, {});
    check("advances toward w2 along +x", near_eq(ai.creature(c)->position().x, 5.0f));
    ai.tick(1000, {});
    check("reaches w2", near_eq(ai.creature(c)->position().x, 10.0f) &&
                            near_eq(ai.creature(c)->position().y, 10.0f));

    // Ping-pong: home (0,0) <-> (6,0), speed 6 m/tick. Bounces at each end.
    CreatureSpawnDef p;
    p.template_id = 2;
    p.level = 2;
    p.faction = Faction::kHostile;
    p.home = at(0, 0);
    p.aggro_base_radius = 0.0f;
    p.leash_radius = 100.0f;
    p.respawn_ms = 1000;
    p.move_speed = 6.0f;
    p.patrol_mode = PatrolMode::kPingPong;
    p.waypoints = {at(0, 0), at(6, 0)};

    CreatureAi pai;
    const ObjectGuid pc = pai.add_spawn(p);
    pai.tick(1000, {});  // at w0 already -> aim at w1
    pai.tick(1000, {});  // reach w1 (6,0)
    check("ping-pong reaches far end", near_eq(pai.creature(pc)->position().x, 6.0f));
    pai.tick(1000, {});  // bounce back toward w0 -> (0,0)
    check("ping-pong bounces back to near end", near_eq(pai.creature(pc)->position().x, 0.0f));
}

// ---------------------------------------------------------------------------
void test_placeholder_spawns() {
    std::printf("G. placeholder greybox spawn set\n");
    CreatureAi ai;
    const std::vector<ObjectGuid> guids = ai.load_placeholder_spawns(at(0, 0));
    check("seeds three placeholder creatures", guids.size() == 3);
    check("size() reflects the spawns", ai.size() == 3);
    bool all_alive_patrolling = true;
    for (ObjectGuid g : guids) {
        if (ai.state_of(g) != AiState::kPatrol) all_alive_patrolling = false;
        if (!ai.creature(g) || !ai.creature(g)->is_alive()) all_alive_patrolling = false;
    }
    check("all spawn alive and patrolling", all_alive_patrolling);
    check("unknown guid -> null creature", ai.creature(0) == nullptr);
}

}  // namespace

int main() {
    std::printf("worldd creature-ai unit test (issues #347 + #348)\n");
    test_aggro_by_level_delta();
    test_threat_ordering();
    test_basic_attack_intents();
    test_leash_evade_heal();
    test_respawn_timing();
    test_waypoint_patrol();
    test_placeholder_spawns();
    std::printf("%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
