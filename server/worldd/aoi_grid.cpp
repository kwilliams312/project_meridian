// SPDX-License-Identifier: Apache-2.0
//
// worldd — Grid / AoI engine v0 implementation (issue #87). See aoi_grid.h for
// the clean-room provenance + the grid/hysteresis design.

#include "aoi_grid.h"

#include <algorithm>
#include <cmath>

#include "movement_constants.h"

namespace meridian::worldd {
namespace {

// Clamp a coordinate into the M0 bootstrap play area [kZoneMinXY, kZoneMaxXY]
// before cell indexing, so an out-of-bounds authoritative position can never
// index outside the grid (defence in depth; the #86 validator already keeps
// positions in-bounds).
float clamp_axis(float v) {
    return std::clamp(v, movement::kZoneMinXY, movement::kZoneMaxXY);
}

}  // namespace

float horizontal_distance(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

CellCoord AoiGrid::cell_of(const Position& pos) const {
    const float x = clamp_axis(pos.x) - movement::kZoneMinXY;
    const float y = clamp_axis(pos.y) - movement::kZoneMinXY;
    CellCoord c;
    c.cx = static_cast<std::int32_t>(std::floor(x / kAoiCellSizeM));
    c.cy = static_cast<std::int32_t>(std::floor(y / kAoiCellSizeM));
    return c;
}

void AoiGrid::upsert(AoiId id, const Position& pos) {
    const CellCoord new_cell = cell_of(pos);

    auto cit = cell_by_id_.find(id);
    if (cit != cell_by_id_.end()) {
        // Already tracked: if its cell changed, move it between buckets.
        if (!(cit->second == new_cell)) {
            const CellKey old_key{cit->second.cx, cit->second.cy};
            auto bit = ids_by_cell_.find(old_key);
            if (bit != ids_by_cell_.end()) {
                bit->second.erase(id);
                if (bit->second.empty()) ids_by_cell_.erase(bit);
            }
            ids_by_cell_[CellKey{new_cell.cx, new_cell.cy}].insert(id);
            cit->second = new_cell;
        }
    } else {
        // New session: bucket it.
        ids_by_cell_[CellKey{new_cell.cx, new_cell.cy}].insert(id);
        cell_by_id_[id] = new_cell;
    }
    positions_[id] = pos;
}

void AoiGrid::remove(AoiId id) {
    auto cit = cell_by_id_.find(id);
    if (cit != cell_by_id_.end()) {
        const CellKey key{cit->second.cx, cit->second.cy};
        auto bit = ids_by_cell_.find(key);
        if (bit != ids_by_cell_.end()) {
            bit->second.erase(id);
            if (bit->second.empty()) ids_by_cell_.erase(bit);
        }
        cell_by_id_.erase(cit);
    }
    positions_.erase(id);
}

bool AoiGrid::contains(AoiId id) const {
    return positions_.find(id) != positions_.end();
}

Position AoiGrid::position_of(AoiId id) const {
    auto it = positions_.find(id);
    return it != positions_.end() ? it->second : Position{};
}

std::unordered_set<AoiId> AoiGrid::interest_set(
    AoiId self, const std::unordered_set<AoiId>& previous) const {
    std::unordered_set<AoiId> out;

    auto self_pos_it = positions_.find(self);
    if (self_pos_it == positions_.end()) return out;  // self not tracked
    const Position& self_pos = self_pos_it->second;
    const CellCoord self_cell = cell_of(self_pos);

    // Visit self's cell + the 8 neighbours (3×3 block). The leave radius fits in
    // one cell (static_assert in the header), so every session within leave
    // range of self lives in this block — a full-map scan is unnecessary.
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const CellKey key{self_cell.cx + dx, self_cell.cy + dy};
            auto bit = ids_by_cell_.find(key);
            if (bit == ids_by_cell_.end()) continue;

            for (AoiId cand : bit->second) {
                if (cand == self) continue;  // never in one's own interest set
                auto cpit = positions_.find(cand);
                if (cpit == positions_.end()) continue;  // defensive
                const float dist = horizontal_distance(self_pos, cpit->second);

                const bool was_visible = previous.find(cand) != previous.end();
                // Hysteresis: enter within the enter radius; only leave beyond
                // the leave radius; sticky in the band between them.
                if (was_visible) {
                    if (dist <= kAoiLeaveRadiusM) out.insert(cand);
                } else {
                    if (dist <= kAoiEnterRadiusM) out.insert(cand);
                }
            }
        }
    }
    return out;
}

std::unordered_set<AoiId> AoiGrid::within_radius(AoiId self, float radius) const {
    std::unordered_set<AoiId> out;
    if (radius <= 0.0f) return out;

    auto self_pos_it = positions_.find(self);
    if (self_pos_it == positions_.end()) return out;  // self not tracked
    const Position& self_pos = self_pos_it->second;
    const CellCoord self_cell = cell_of(self_pos);

    // Size the neighbour block to the requested radius (no hysteresis, one-shot):
    // a candidate up to `radius` away can be at most ceil(radius / cell) cells
    // off in either axis, so a (2k+1)×(2k+1) block is exactly sufficient. This is
    // the SAD §2.5 cell-visitor "notify observers within R of P" — a wide yell
    // (k > 1) and a tight say (k = 1) are both covered.
    const std::int32_t k =
        static_cast<std::int32_t>(std::ceil(radius / kAoiCellSizeM));

    for (std::int32_t dy = -k; dy <= k; ++dy) {
        for (std::int32_t dx = -k; dx <= k; ++dx) {
            const CellKey key{self_cell.cx + dx, self_cell.cy + dy};
            auto bit = ids_by_cell_.find(key);
            if (bit == ids_by_cell_.end()) continue;

            for (AoiId cand : bit->second) {
                if (cand == self) continue;  // never notify oneself via the grid
                auto cpit = positions_.find(cand);
                if (cpit == positions_.end()) continue;  // defensive
                if (horizontal_distance(self_pos, cpit->second) <= radius) {
                    out.insert(cand);
                }
            }
        }
    }
    return out;
}

}  // namespace meridian::worldd
