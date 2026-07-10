// SPDX-License-Identifier: Apache-2.0
//
// worldd — player death state machine implementation (issue #359, CMB-03). See
// death_state.h for the clean-room provenance, the FSM, and the world-data seam.

#include "death_state.h"

#include <algorithm>
#include <cmath>

namespace meridian::worldd {

const char* death_phase_name(DeathPhase p) {
    switch (p) {
        case DeathPhase::kAlive:  return "ALIVE";
        case DeathPhase::kCorpse: return "CORPSE";
        case DeathPhase::kGhost:  return "GHOST";
    }
    return "?";
}

namespace {

// Planar (x,y) squared distance — z is the flat-ground plane at M1 (movement
// validation), so the corpse-run check is 2-D. Squared to avoid a sqrt.
float planar_dist_sq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace

ObjectGuid DeathStateMachine::on_death(ObjectGuid player_guid, const Position& death_pos,
                                       const Position& graveyard_pos) {
    const ObjectGuid corpse_guid = next_corpse_guid_++;

    corpses_.insert_or_assign(corpse_guid, Corpse(corpse_guid, death_pos, player_guid));

    DeathRecord rec;
    rec.phase = DeathPhase::kCorpse;
    rec.corpse_guid = corpse_guid;
    rec.corpse_pos = death_pos;
    rec.graveyard_pos = graveyard_pos;
    rec.release_remaining_ms = cfg_.auto_release_ms;
    records_.insert_or_assign(player_guid, rec);

    return corpse_guid;
}

bool DeathStateMachine::request_release(ObjectGuid player_guid) {
    auto it = records_.find(player_guid);
    if (it == records_.end() || it->second.phase != DeathPhase::kCorpse) return false;
    it->second.phase = DeathPhase::kGhost;
    it->second.release_remaining_ms = 0;
    return true;
}

void DeathStateMachine::advance(std::uint32_t dt_ms, std::vector<ObjectGuid>& auto_released) {
    for (auto& kv : records_) {
        DeathRecord& r = kv.second;
        if (r.phase != DeathPhase::kCorpse) continue;
        if (r.release_remaining_ms > dt_ms) {
            r.release_remaining_ms -= dt_ms;
            continue;
        }
        // Timer elapsed → auto-release to the graveyard.
        r.release_remaining_ms = 0;
        r.phase = DeathPhase::kGhost;
        auto_released.push_back(kv.first);
    }
    // Deterministic order for the caller's logging / broadcast.
    std::sort(auto_released.begin(), auto_released.end());
}

bool DeathStateMachine::can_resurrect(ObjectGuid player_guid, const Position& at_pos,
                                      ResurrectReject& why) const {
    auto it = records_.find(player_guid);
    if (it == records_.end()) {
        why = ResurrectReject::kNotDead;
        return false;
    }
    const DeathRecord& r = it->second;
    if (r.phase == DeathPhase::kCorpse) {
        why = ResurrectReject::kNotReleased;  // release to the graveyard first
        return false;
    }
    // kGhost: the corpse-run must be complete (standing at the corpse).
    const float radius_sq = cfg_.corpse_run_radius_m * cfg_.corpse_run_radius_m;
    if (planar_dist_sq(at_pos, r.corpse_pos) > radius_sq) {
        why = ResurrectReject::kTooFar;
        return false;
    }
    why = ResurrectReject::kNone;
    return true;
}

ObjectGuid DeathStateMachine::resurrect(ObjectGuid player_guid) {
    auto it = records_.find(player_guid);
    if (it == records_.end()) return 0;
    const ObjectGuid corpse_guid = it->second.corpse_guid;
    corpses_.erase(corpse_guid);
    records_.erase(it);
    return corpse_guid;
}

std::uint32_t DeathStateMachine::resurrect_health(std::uint32_t max_health) const {
    const std::uint32_t cap = std::max<std::uint32_t>(1, max_health);
    const std::uint64_t hp = static_cast<std::uint64_t>(cap) * cfg_.resurrect_health_pct / 100;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(hp, 1, cap));
}

DeathPhase DeathStateMachine::phase_of(ObjectGuid player_guid) const {
    auto it = records_.find(player_guid);
    return it == records_.end() ? DeathPhase::kAlive : it->second.phase;
}

const DeathRecord* DeathStateMachine::record(ObjectGuid player_guid) const {
    auto it = records_.find(player_guid);
    return it == records_.end() ? nullptr : &it->second;
}

const Corpse* DeathStateMachine::corpse(ObjectGuid corpse_guid) const {
    auto it = corpses_.find(corpse_guid);
    return it == corpses_.end() ? nullptr : &it->second;
}

}  // namespace meridian::worldd
