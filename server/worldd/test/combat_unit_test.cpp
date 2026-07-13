// SPDX-License-Identifier: Apache-2.0
//
// worldd — Combat Unit model UNIT TEST (issue #342, CMB-01 foundation).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5/§9 (the shallow
// WorldObject→Unit→{Player,Creature} hierarchy, NOT an ECS) and combat_unit.h /
// world_state.h only. No GPL / emulator source consulted (CONTRIBUTING.md).
//
// PURE / DB-FREE / SOCKET-FREE: exercises the model + lifecycle directly, plus
// the WorldState wiring (a session's Unit reachable via unit_for_slot). Runs in
// the plain server ctest — the "model must be DB-free unit-testable" ask (#342).
//
// What it proves:
//   A. HIERARCHY: WorldObject→Unit→{Player,Creature}; leaf types stamp their
//      ObjectType and specialise (Player class/account, Creature template/home);
//      GameObject/Corpse stubs exist and carry their id.
//   B. SPAWN: a Unit spawns alive at full health/resource from its stats.
//   C. DAMAGE→HEALTH: damage reduces health, clamps at 0, reports applied +
//      lethality; damage to a dead unit is a no-op.
//   D. DEATH TRANSITION: health hitting 0 flips LifeState alive→dead exactly once;
//      kill() forces it.
//   E. HEAL / RESURRECT: healing clamps at max and refuses a corpse; resurrect
//      brings a dead unit back clamped into [1,max].
//   F. RESOURCE: all-or-nothing spend; restore clamps at max.
//   G. PLACEHOLDER STATS: class-flavored player stats + level scaling; creature
//      stats scale and are resourceless.
//   H. WORLDSTATE WIRING: a session entered into WorldState owns a Unit reachable
//      via unit_for_slot; damaging it to 0 transitions it to dead; the pointer is
//      null for an unknown slot and after the session leaves.

#include "combat_unit.h"
#include "movement_constants.h"
#include "movement_validation.h"  // Position
#include "world_state.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace mc = meridian::worldd::movement;
namespace mn = meridian::net;

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

// A UnitStats fixture with round numbers so the arithmetic is obvious.
UnitStats fixture_stats() {
    UnitStats s;
    s.level = 5;
    s.max_health = 200;
    s.resource_type = ResourceType::kMana;
    s.max_resource = 100;
    s.faction = Faction::kPlayer;
    return s;
}

}  // namespace

int main() {
    std::printf("worldd combat-unit model test (#342)\n");

    // ===== A. HIERARCHY =====================================================
    {
        Player p(0x1001, at(10, 20), fixture_stats(), /*account=*/42,
                 /*char_class=*/static_cast<std::uint8_t>(1), "Aldric");
        check("A: Player type is kPlayer", p.type() == ObjectType::kPlayer);
        check("A: Player guid", p.guid() == 0x1001);
        check("A: Player carries account/class/name",
              p.account_id() == 42 && p.char_class() == 1 && p.name() == "Aldric");
        check("A: Player IS-A Unit (health accessor)", p.max_health() == 200);
        // Position lives on the WorldObject base.
        check("A: WorldObject position", p.position().x == 10 && p.position().y == 20);

        Creature c(0x2002, at(30, 40), placeholder_creature_stats(3), /*template=*/777);
        check("A: Creature type is kCreature", c.type() == ObjectType::kCreature);
        check("A: Creature template id", c.template_id() == 777);
        check("A: Creature spawn_home is its spawn position",
              c.spawn_home().x == 30 && c.spawn_home().y == 40);

        GameObject go(0x3003, at(1, 2), /*template=*/55);
        check("A: GameObject stub type + template",
              go.type() == ObjectType::kGameObject && go.template_id() == 55);
        Corpse corpse(0x4004, at(3, 4), /*owner=*/0x1001);
        check("A: Corpse stub type + owner",
              corpse.type() == ObjectType::kCorpse && corpse.owner_guid() == 0x1001);

        // Polymorphism: hold leaves behind Unit* / WorldObject*.
        Unit* up = &p;
        WorldObject* wp = &c;
        check("A: Unit* sees Player health", up->max_health() == 200);
        check("A: WorldObject* sees Creature guid", wp->guid() == 0x2002);
    }

    // ===== B. SPAWN =========================================================
    {
        Player p(1, at(0, 0), fixture_stats(), 0, 1, "");
        check("B: spawns alive", p.is_alive() && !p.is_dead());
        check("B: spawns at full health", p.health() == p.max_health());
        check("B: spawns at full resource",
              p.resource() == p.max_resource() && p.resource() == 100);
        check("B: level/faction from stats",
              p.level() == 5 && p.faction() == Faction::kPlayer);

        // A resourceless unit has a zeroed pool regardless of stats.max_resource.
        UnitStats none = fixture_stats();
        none.resource_type = ResourceType::kNone;
        Unit u(9, ObjectType::kCreature, at(0, 0), none);
        check("B: kNone resource pool is 0",
              u.resource_type() == ResourceType::kNone && u.max_resource() == 0 &&
                  u.resource() == 0);

        // max_health is coerced to >= 1 even from a 0 stat.
        UnitStats zero = fixture_stats();
        zero.max_health = 0;
        Unit z(10, ObjectType::kCreature, at(0, 0), zero);
        check("B: max_health coerced to >= 1", z.max_health() == 1 && z.health() == 1);
    }

    // ===== C. DAMAGE → HEALTH ==============================================
    {
        Player p(1, at(0, 0), fixture_stats(), 0, 1, "");  // 200 HP
        DamageResult r1 = p.apply_damage(30);
        check("C: damage applied", r1.applied == 30 && !r1.lethal);
        check("C: health reduced", p.health() == 170);

        // Overkill clamps applied to remaining health and does not underflow.
        DamageResult r2 = p.apply_damage(1000);
        check("C: overkill applied clamps to remaining", r2.applied == 170);
        check("C: health floors at 0", p.health() == 0);
        check("C: overkill is lethal", r2.lethal);

        // Damage on a corpse is a no-op.
        DamageResult r3 = p.apply_damage(50);
        check("C: damage on dead unit is no-op",
              r3.applied == 0 && !r3.lethal && p.health() == 0);
    }

    // ===== D. DEATH TRANSITION =============================================
    {
        Player p(1, at(0, 0), fixture_stats(), 0, 1, "");
        check("D: starts alive", p.is_alive());
        DamageResult lethal = p.apply_damage(p.max_health());
        check("D: exact-lethal damage kills", lethal.lethal && p.is_dead());

        // kill() forces death from full health, and is idempotent.
        Player q(2, at(0, 0), fixture_stats(), 0, 1, "");
        q.kill();
        check("D: kill() forces dead + 0 HP", q.is_dead() && q.health() == 0);
        q.kill();
        check("D: kill() idempotent", q.is_dead());
    }

    // ===== E. HEAL / RESURRECT =============================================
    {
        Player p(1, at(0, 0), fixture_stats(), 0, 1, "");  // 200 HP
        p.apply_damage(150);  // 50 HP
        std::uint32_t healed = p.apply_healing(30);
        check("E: heal applies", healed == 30 && p.health() == 80);
        std::uint32_t over = p.apply_healing(1000);
        check("E: heal clamps at max", over == 120 && p.health() == 200);

        p.kill();
        check("E: heal refuses a corpse",
              p.apply_healing(50) == 0 && p.health() == 0 && p.is_dead());

        p.resurrect(75);
        check("E: resurrect brings back alive with given HP",
              p.is_alive() && p.health() == 75);

        // resurrect on a live unit is a no-op; resurrect clamps into [1,max].
        p.resurrect(10);
        check("E: resurrect no-op when alive", p.health() == 75);
        Player d(2, at(0, 0), fixture_stats(), 0, 1, "");
        d.kill();
        d.resurrect(0);  // 0 coerced to 1
        check("E: resurrect(0) coerces to 1 HP", d.is_alive() && d.health() == 1);
        Player e(3, at(0, 0), fixture_stats(), 0, 1, "");
        e.kill();
        e.resurrect(99999);  // clamped to max
        check("E: resurrect clamps to max", e.health() == 200);
    }

    // ===== F. RESOURCE =====================================================
    {
        Player p(1, at(0, 0), fixture_stats(), 0, 1, "");  // 100 mana
        check("F: spend within pool succeeds", p.spend_resource(40) && p.resource() == 60);
        check("F: over-spend is all-or-nothing (refused)",
              !p.spend_resource(1000) && p.resource() == 60);
        p.restore_resource(1000);
        check("F: restore clamps at max", p.resource() == 100);
    }

    // ===== G. PLACEHOLDER STATS ============================================
    {
        UnitStats van = placeholder_player_stats(/*Vanguard=*/1, 1);
        UnitStats run = placeholder_player_stats(/*Runcaller=*/2, 1);
        check("G: player stats are kPlayer faction", van.faction == Faction::kPlayer);
        check("G: Vanguard uses rage", van.resource_type == ResourceType::kRage);
        check("G: Runcaller uses mana", run.resource_type == ResourceType::kMana);
        check("G: melee is tankier than caster", van.max_health > run.max_health);

        UnitStats van10 = placeholder_player_stats(1, 10);
        check("G: health scales with level", van10.max_health > van.max_health);

        UnitStats unknown = placeholder_player_stats(/*unset=*/0, 1);
        check("G: unknown class falls back (no resource)",
              unknown.resource_type == ResourceType::kNone && unknown.max_health > 0);

        UnitStats mob = placeholder_creature_stats(5);
        UnitStats mob1 = placeholder_creature_stats(1);
        check("G: creature is resourceless + hostile",
              mob.resource_type == ResourceType::kNone && mob.faction == Faction::kHostile);
        check("G: creature health scales with level", mob.max_health > mob1.max_health);
    }

    // ===== H. WORLDSTATE WIRING ============================================
    {
        WorldState world;
        EntityIdentity id;
        id.entity_guid = 0;  // D-11 placeholder stub → synthetic guid assigned
        id.type_id = 1;
        id.char_class = 1;  // Vanguard
        // A no-op egress (single-session: no observers, nothing is emitted).
        EnterResult er = world.enter(id, at(-320, -320),
                                     [](mn::Opcode, const std::vector<std::uint8_t>&) {
                                         return true;
                                     });
        check("H: session entered", world.session_count() == 1);

        Unit* u = world.unit_for_slot(er.slot);
        check("H: unit reachable via unit_for_slot", u != nullptr);
        check("H: unknown slot yields nullptr", world.unit_for_slot(999999) == nullptr);
        if (u != nullptr) {
            check("H: session unit is a Player", u->type() == ObjectType::kPlayer);
            check("H: session unit spawns alive at full HP",
                  u->is_alive() && u->health() == u->max_health() && u->health() > 0);
            check("H: session unit position is its spawn", u->position().x == -320);

            // Damage it to death through the WorldState-owned Unit.
            DamageResult killed = u->apply_damage(u->max_health());
            check("H: damaging the session unit to 0 kills it",
                  killed.lethal && u->is_dead());
        }

        world.leave(er.slot);
        check("H: after leave, unit_for_slot is nullptr",
              world.unit_for_slot(er.slot) == nullptr && world.session_count() == 0);
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD COMBAT-UNIT TESTS PASSED\n"
                            : "\n%d WORLDD COMBAT-UNIT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
