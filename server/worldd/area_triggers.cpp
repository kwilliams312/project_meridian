// SPDX-License-Identifier: Apache-2.0
//
// worldd — area triggers + POI discovery core implementation (issue #368;
// WLD-01/03, epic #20). See area_triggers.h for the clean-room provenance, the
// volume model, and the evaluation contract.

#include "area_triggers.h"

#include "movement_constants.h"  // kZoneMinXY / kZoneMaxXY / kFlatGroundZ (D-19 map)

namespace meridian::worldd {
namespace mc = meridian::worldd::movement;

bool TriggerVolume::contains(const Position& p) const {
    return p.x >= min_x && p.x <= max_x &&
           p.y >= min_y && p.y <= max_y &&
           p.z >= min_z && p.z <= max_z;
}

void AreaTriggerSet::load(std::vector<TriggerVolume> volumes) {
    volumes_ = std::move(volumes);
}

std::vector<TriggerEvent> AreaTriggerSet::evaluate(AoiId guid, const Position& pos) {
    std::vector<TriggerEvent> events;

    // The character's current occupancy + discovered sets (created empty on first
    // touch; dropped by remove() on world-leave).
    std::unordered_set<TriggerId>& inside = occupancy_[guid];
    std::unordered_set<TriggerId>& discovered = discovered_[guid];

    // Volume-load order gives a deterministic event order for seeded tests.
    for (const TriggerVolume& v : volumes_) {
        const bool now_in = v.contains(pos);
        const bool was_in = inside.find(v.id) != inside.end();

        if (now_in && !was_in) {
            // Crossed IN → one ENTER event. A discovery volume the character has
            // never discovered additionally marks + notifies exactly once.
            inside.insert(v.id);
            TriggerEvent e;
            e.trigger_id = v.id;
            e.kind = v.kind;
            e.area_id = v.area_id;
            e.name_id = v.name_id;
            e.entered = true;
            if (v.kind == TriggerKind::kDiscovery &&
                discovered.find(v.id) == discovered.end()) {
                discovered.insert(v.id);
                e.discovered_now = true;
            }
            events.push_back(e);
        } else if (!now_in && was_in) {
            // Crossed OUT → one LEAVE event. `discovered` is intentionally NOT
            // cleared here, so a later re-entry does not re-fire discovery.
            inside.erase(v.id);
            TriggerEvent e;
            e.trigger_id = v.id;
            e.kind = v.kind;
            e.area_id = v.area_id;
            e.name_id = v.name_id;
            e.entered = false;
            events.push_back(e);
        }
        // now_in == was_in → no crossing, no event (steady state inside/outside).
    }

    return events;
}

void AreaTriggerSet::remove(AoiId guid) {
    occupancy_.erase(guid);
    discovered_.erase(guid);
}

bool AreaTriggerSet::is_inside(AoiId guid, TriggerId id) const {
    auto it = occupancy_.find(guid);
    return it != occupancy_.end() && it->second.find(id) != it->second.end();
}

bool AreaTriggerSet::is_discovered(AoiId guid, TriggerId id) const {
    auto it = discovered_.find(guid);
    return it != discovered_.end() && it->second.find(id) != it->second.end();
}

// ---------------------------------------------------------------------------
// M1 placeholder volume set (the mcc #28 seam).
// ---------------------------------------------------------------------------
//
// Three deterministic volumes on the 128 m flat bootstrap chunk (D-19,
// kZoneMinXY..kZoneMaxXY = [0, 128]), each clear of the play-area-centre spawn
// (64, 64) so a fresh login does not instantly trip a trigger. Ids/area/name ids
// are placeholder content ids; real values arrive with compiled world data.
//   • id=1  NE corner  — DISCOVERY POI (the one client-facing trigger): entering
//                        marks it discovered on the character + sends POI_DISCOVERED.
//   • id=2  SW corner  — GRAVEYARD zone (server-side event only at M1).
//   • id=3  NW corner  — QUEST-OBJECTIVE hook volume (server-side event only).
std::vector<TriggerVolume> placeholder_area_triggers() {
    const float z_lo = mc::kFlatGroundZ - 1000.0f;  // span the whole flat map in z
    const float z_hi = mc::kFlatGroundZ + 1000.0f;

    std::vector<TriggerVolume> v;

    TriggerVolume discovery;
    discovery.id = 1;
    discovery.kind = TriggerKind::kDiscovery;
    discovery.area_id = 100;
    discovery.name_id = 1;
    discovery.min_x = 90.0f;
    discovery.max_x = 110.0f;
    discovery.min_y = 90.0f;
    discovery.max_y = 110.0f;
    discovery.min_z = z_lo;
    discovery.max_z = z_hi;
    v.push_back(discovery);

    TriggerVolume graveyard;
    graveyard.id = 2;
    graveyard.kind = TriggerKind::kGraveyard;
    graveyard.area_id = 100;
    graveyard.name_id = 2;
    graveyard.min_x = 10.0f;
    graveyard.max_x = 30.0f;
    graveyard.min_y = 10.0f;
    graveyard.max_y = 30.0f;
    graveyard.min_z = z_lo;
    graveyard.max_z = z_hi;
    v.push_back(graveyard);

    TriggerVolume quest;
    quest.id = 3;
    quest.kind = TriggerKind::kQuestObjective;
    quest.area_id = 101;
    quest.name_id = 3;
    quest.min_x = 10.0f;
    quest.max_x = 30.0f;
    quest.min_y = 90.0f;
    quest.max_y = 110.0f;
    quest.min_z = z_lo;
    quest.max_z = z_hi;
    v.push_back(quest);

    return v;
}

}  // namespace meridian::worldd
