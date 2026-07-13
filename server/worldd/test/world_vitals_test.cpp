// SPDX-License-Identifier: Apache-2.0
//
// worldd — unit VITALS broadcast UNIT TEST (issue #430, UI-01 HUD contract; the
// server prerequisite for the client HUD epic #24). The deterministic proof that
// the wire carries per-unit vitals (health/power/level/name) server-authoritatively.
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 (the shallow Unit hierarchy
// owns health/power/level; "authoritative state → interest set → per-subscriber
// egress" AoI relay), the world.fbs IF-2 contract (EntityEnter vitals block +
// VitalsUpdate), and world_state.h / combat_unit.h only. No GPL source consulted
// (CONTRIBUTING).
//
// PURE / DB-FREE / SOCKET-FREE / SEEDED: drives WorldState directly with CAPTURING
// egress sinks (the EgressFn is a std::function, so a test lambda records exactly
// the frames the relay emits — no TLS, no MariaDB). Positions/classes/names are
// fixed, so every assertion is deterministic. Runs in the plain server ctest. The
// DB-backed two-client wire proof is world_relay_test (SKIPs without a DB); the
// vitals LOGIC is proven here.
//
// What it proves (the story's acceptance list):
//   1. An entity entering AoI carries correct vitals — the EntityEnter an observer
//      receives for a newcomer holds its health/max, power/max + type, level, name.
//   2. A damage change moves health and emits a VITALS_UPDATE with the new value to
//      the subject's own client AND every AoI observer.
//   3. A heal change likewise pushes the restored health.
//   4. A dead unit reports health 0 in its VITALS_UPDATE.
//   5. A level-up updates level + max_health/max_power in the VITALS_UPDATE.

#include "combat_unit.h"          // Unit / placeholder_player_stats / ResourceType
#include "movement_constants.h"
#include "movement_validation.h"  // Position
#include "world_generated.h"
#include "world_state.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace meridian::worldd;
namespace fb = flatbuffers;
namespace mn = meridian::net;
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

// A decoded EntityEnter's vitals block captured by an egress sink.
struct Enter {
    bool got = false;
    std::uint64_t guid = 0;
    std::uint32_t health = 0, max_health = 0, power = 0, max_power = 0;
    mn::PowerType power_type = mn::PowerType::NONE;
    std::uint16_t level = 0;
    std::string name;
};

// A decoded VitalsUpdate captured by an egress sink.
struct Vitals {
    bool got = false;
    std::uint64_t guid = 0;
    std::uint32_t health = 0, max_health = 0, power = 0, max_power = 0;
    mn::PowerType power_type = mn::PowerType::NONE;
    std::uint16_t level = 0;
};

// A capturing egress sink: records the EntityEnter + VitalsUpdate frames the relay
// emits to this session (ignores ENTITY_UPDATE/LEAVE and any other opcode). The
// payload is the bare FlatBuffer table (world_state emits table bytes; the IF-2
// header is added by the real SessionEgress, absent here).
struct Capture {
    std::vector<Enter> enters;
    std::vector<Vitals> vitals;

    EgressFn sink() {
        return [this](mn::Opcode op, const std::vector<std::uint8_t>& payload) -> bool {
            if (op == mn::Opcode::ENTITY_ENTER) {
                fb::Verifier v(payload.data(), payload.size());
                if (!v.VerifyBuffer<mn::EntityEnter>(nullptr)) return false;
                const auto* e = fb::GetRoot<mn::EntityEnter>(payload.data());
                enters.push_back(Enter{true, e->entity_guid(), e->health(), e->max_health(),
                                       e->power(), e->max_power(), e->power_type(),
                                       e->level(), e->name() ? e->name()->str() : std::string()});
            } else if (op == mn::Opcode::VITALS_UPDATE) {
                fb::Verifier v(payload.data(), payload.size());
                if (!v.VerifyBuffer<mn::VitalsUpdate>(nullptr)) return false;
                const auto* u = fb::GetRoot<mn::VitalsUpdate>(payload.data());
                vitals.push_back(Vitals{true, u->entity_guid(), u->health(), u->max_health(),
                                        u->power(), u->max_power(), u->power_type(),
                                        u->level()});
            }
            return true;  // ignore other relay frames
        };
    }
};

// The EntityEnter this capture holds for `guid` (or a defaulted Enter if none).
Enter enter_for(const Capture& c, std::uint64_t guid) {
    for (const auto& e : c.enters)
        if (e.guid == guid) return e;
    return Enter{};
}

// The LAST VitalsUpdate this capture holds for `guid` (or a defaulted Vitals).
Vitals last_vitals_for(const Capture& c, std::uint64_t guid) {
    for (auto it = c.vitals.rbegin(); it != c.vitals.rend(); ++it)
        if (it->guid == guid) return *it;
    return Vitals{};
}

std::size_t vitals_count_for(const Capture& c, std::uint64_t guid) {
    std::size_t n = 0;
    for (const auto& u : c.vitals)
        if (u.guid == guid) ++n;
    return n;
}

}  // namespace

int main() {
    std::printf("worldd unit vitals broadcast test (UI-01 HUD contract, #430)\n");

    WorldState world;
    Capture ca, cb;

    // A = Runcaller (class 2 -> mana pool); B = Vanguard (class 1 -> rage pool).
    // Distinct guids + names; co-located at the zone centre so they are mutually
    // in AoI on enter (enter() relays a bidirectional EntityEnter).
    constexpr std::uint64_t kGuidA = 2001, kGuidB = 2002;
    EntityIdentity ida;
    ida.entity_guid = kGuidA;
    ida.char_class = 2;  // Runcaller (mana)
    ida.name = "Aldric";
    EntityIdentity idb;
    idb.entity_guid = kGuidB;
    idb.char_class = 1;  // Vanguard (rage)
    idb.name = "Brynn";

    world.enter(ida, at(-320.0f, -320.0f), ca.sink());
    world.enter(idb, at(-320.0f, -320.0f), cb.sink());

    // ===== 1. Entity entering AoI carries correct vitals =====================
    // On B's enter, B's capture (cb) received an EntityEnter for A (guid A). Its
    // vitals must match A's authoritative Unit (read from the world, so the test is
    // robust to the placeholder curve's exact numbers) — health/max, power/max,
    // power_type=MANA (Runcaller), level 1, name "Aldric".
    const Unit* a_unit = world.unit_for_guid(kGuidA);
    check("1: A's unit exists in the world", a_unit != nullptr);
    const Enter ea = enter_for(cb, kGuidA);
    check("1: B received an EntityEnter for A", ea.got);
    if (a_unit != nullptr) {
        check("1: EntityEnter health matches A's unit", ea.health == a_unit->health());
        check("1: EntityEnter max_health matches", ea.max_health == a_unit->max_health());
        check("1: EntityEnter power matches A's mana", ea.power == a_unit->resource());
        check("1: EntityEnter max_power matches", ea.max_power == a_unit->max_resource());
        check("1: EntityEnter health is full at spawn", ea.health == ea.max_health);
    }
    check("1: EntityEnter power_type is MANA (Runcaller)", ea.power_type == mn::PowerType::MANA);
    check("1: EntityEnter level is 1", ea.level == 1);
    check("1: EntityEnter name is A's name", ea.name == "Aldric");

    // Reciprocal: A received an EntityEnter for B (Vanguard -> RAGE).
    const Enter eb = enter_for(ca, kGuidB);
    check("1: A received an EntityEnter for B", eb.got);
    check("1: B's EntityEnter power_type is RAGE (Vanguard)", eb.power_type == mn::PowerType::RAGE);
    check("1: B's EntityEnter name is B's name", eb.name == "Brynn");

    // ===== 2. A damage change moves health + emits a VITALS_UPDATE ===========
    Unit* a_mut = world.unit_for_guid(kGuidA);
    const std::uint32_t a_full = a_mut->max_health();
    a_mut->apply_damage(30);
    const std::uint32_t a_after_dmg = a_mut->health();  // full - 30 (>0)
    const std::size_t recips = world.broadcast_vitals(kGuidA);
    check("2: broadcast reached A (self) + B (observer) = 2", recips == 2);
    {
        const Vitals vb = last_vitals_for(cb, kGuidA);
        check("2: B (observer) got a VITALS_UPDATE for A", vb.got);
        check("2: the delta carries A's reduced health", vb.health == a_after_dmg);
        check("2: the delta health is full-30", vb.health == a_full - 30);
        check("2: the delta max_health is unchanged", vb.max_health == a_full);
        check("2: the delta still carries A's power_type", vb.power_type == mn::PowerType::MANA);
        const Vitals va = last_vitals_for(ca, kGuidA);
        check("2: A (self) also got the VITALS_UPDATE", va.got && va.health == a_after_dmg);
    }

    // ===== 3. A heal change pushes the restored health ======================
    a_mut->apply_healing(10);
    const std::uint32_t a_after_heal = a_mut->health();  // +10
    world.broadcast_vitals(kGuidA);
    {
        const Vitals vb = last_vitals_for(cb, kGuidA);
        check("3: heal delta carries the restored health", vb.health == a_after_heal);
        check("3: heal restored +10 over the damaged value", vb.health == a_after_dmg + 10);
    }

    // ===== 4. A dead unit reports health 0 ==================================
    a_mut->kill();  // health -> 0, life -> dead
    world.broadcast_vitals(kGuidA);
    {
        const Vitals vb = last_vitals_for(cb, kGuidA);
        check("4: a dead unit's VITALS_UPDATE reports health 0", vb.health == 0);
        check("4: max_health is still reported on death", vb.max_health == a_full);
    }

    // ===== 5. A level-up updates level + max_health/max_power ================
    // Simulate CHR-03 level-up stat growth on B (still alive): bump to level 5 with
    // the placeholder curve's caps, top off, then broadcast. The delta must carry
    // the NEW level + the grown caps (server-authoritative — the client displays it).
    Unit* b_mut = world.unit_for_guid(kGuidB);
    const UnitStats grown = placeholder_player_stats(/*Vanguard=*/1, /*level=*/5);
    b_mut->set_level(5);
    b_mut->set_max_health(grown.max_health);
    b_mut->set_max_resource(grown.max_resource);
    b_mut->apply_healing(grown.max_health);       // heal to the new full
    b_mut->restore_resource(grown.max_resource);  // top off the grown pool
    const std::size_t b_recips = world.broadcast_vitals(kGuidB);
    check("5: level-up broadcast reached B (self) + A (observer) = 2", b_recips == 2);
    {
        const Vitals va = last_vitals_for(ca, kGuidB);  // A observes B
        check("5: A got B's level-up VITALS_UPDATE", va.got);
        check("5: the delta carries B's new level (5)", va.level == 5);
        check("5: the delta carries B's grown max_health", va.max_health == grown.max_health);
        check("5: the delta carries B's grown max_power", va.max_power == grown.max_resource);
        check("5: B's power_type is still RAGE", va.power_type == mn::PowerType::RAGE);
    }
    // Exactly one VITALS_UPDATE per subject per broadcast reached each observer:
    // A observed B's single level-up push (not A's earlier damage/heal/death, which
    // were for guid A).
    check("5: A received exactly one VITALS_UPDATE for B", vitals_count_for(ca, kGuidB) == 1);

    std::printf(g_fail == 0 ? "\nALL WORLDD VITALS TESTS PASSED\n"
                            : "\n%d WORLDD VITALS TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
