// SPDX-License-Identifier: Apache-2.0
//
// Story #784 integration: authored hostile combat profiles flow through
// CreatureAi intents into MapTick's authoritative combat/death path.
// CLEAN-ROOM: designed from Project Meridian's server SAD and the approved #784
// armor contract. No GPL/emulator source consulted.

#include "ability_store.h"
#include "death_state.h"
#include "map_tick.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::worldd;

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
    return p;
}

CreatureSpawnDef authored_mob() {
    CreatureSpawnDef d;
    d.template_id = 200;
    d.level = 1;
    d.faction = Faction::kHostile;
    UnitStats stats;
    stats.level = 1;
    stats.max_health = 100;
    stats.armor = 5;
    stats.faction = Faction::kHostile;
    d.authored_stats = stats;
    d.behavior = CreatureBehavior::kAggressive;
    d.damage_min = 30;
    d.damage_max = 30;
    d.attack_speed_ms = 1000;
    d.home = at(0, 0);
    d.aggro_base_radius = 8.0f;
    d.leash_radius = 18.0f;
    d.move_speed = 3.0f;
    d.respawn_ms = 3000;
    return d;
}

std::size_t count_lines(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}

}  // namespace

int main() {
    std::printf("worldd authored mob basic-attack integration test (#784)\n");
    AbilityStore abilities = load_placeholder_ability_store();

    // Cadence is owned by AI but resolved/logged by the combat phase. With 100ms
    // ticks: tick 1 aggros, tick 2 swings immediately, then ticks 12 and 22.
    {
        MapTick tick(abilities, /*seed=*/784, /*dt_ms=*/100);
        tick.set_report_vitals(true);
        UnitStats player;
        player.max_health = 1000;
        player.armor = 100;
        player.faction = Faction::kPlayer;
        tick.add_player(42, at(1, 0), player);
        tick.add_creature(authored_mob());
        std::vector<TickEvent> events;
        for (int i = 0; i < 31; ++i) {
            std::vector<TickEvent> step = tick.advance();
            events.insert(events.end(), step.begin(), step.end());
        }
        check("authored cadence emits exactly three server attacks",
              count_lines(tick.log_text(), "basic_attack attacker=") == 3);
        check("attacks run through target effective armor",
              tick.unit_for_guid(42)->health() > 900 &&
                  tick.unit_for_guid(42)->health() < 1000);
        bool saw_vitals = false;
        for (const TickEvent& event : events) {
            if (event.kind == TickEventKind::kVitalsChanged &&
                event.vitals.guid == 42 && event.vitals.health < 1000) {
                saw_vitals = true;
            }
        }
        check("mob damage emits authoritative vitals protocol state", saw_vitals);
    }

    // Lethal basic damage must enter the same player corpse/death FSM as every
    // other server damage source; no client request participates.
    {
        MapTick tick(abilities, /*seed=*/785, /*dt_ms=*/100);
        UnitStats player;
        player.max_health = 40;
        player.faction = Faction::kPlayer;
        tick.add_player(43, at(1, 0), player);
        tick.add_creature(authored_mob());
        for (int i = 0; i < 100 && tick.unit_for_guid(43)->is_alive(); ++i)
            tick.advance();
        check("authoritative mob attack kills the player", tick.unit_for_guid(43)->is_dead());
        check("lethal mob attack enters player death flow",
              tick.deaths().phase_of(43) == DeathPhase::kCorpse);
        check("death event attributes the server creature",
              tick.log_text().find("death guid=43 by=") != std::string::npos);
    }

    // A passive friendly with the same required combat fields never attacks.
    {
        MapTick tick(abilities, /*seed=*/786, /*dt_ms=*/100);
        UnitStats player;
        player.max_health = 100;
        player.faction = Faction::kPlayer;
        tick.add_player(44, at(1, 0), player);
        CreatureSpawnDef npc = authored_mob();
        npc.faction = Faction::kFriendly;
        npc.behavior = CreatureBehavior::kPassive;
        npc.authored_stats->faction = Faction::kFriendly;
        tick.add_creature(npc);
        tick.advance(100);
        check("friendly passive profile produces no attacks",
              tick.unit_for_guid(44)->health() == 100 &&
                  count_lines(tick.log_text(), "basic_attack attacker=") == 0);
    }

    std::printf("%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
