// SPDX-License-Identifier: Apache-2.0
//
// worldd — Grid / AoI engine v0 implementation (issue #87). See aoi_grid.h for
// the clean-room provenance + the grid/hysteresis design.

#include "aoi_grid.h"

#include <algorithm>
#include <cmath>

namespace meridian::worldd {

float horizontal_distance(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

CellCoord AoiGrid::cell_of(const Position& pos) const {
    // Clamp into the zone play area [origin, origin+extent] before indexing, so an
    // out-of-bounds authoritative position can never index outside the grid
    // (defence in depth; the #86 validator already keeps positions in-bounds). Then
    // derive the cell index RELATIVE TO THE ORIGIN (so a non-(0,0) origin indexes
    // from cell 0 at the origin corner — #559).
    const float x = std::clamp(pos.x, cfg_.origin_x, cfg_.max_x()) - cfg_.origin_x;
    const float y = std::clamp(pos.y, cfg_.origin_y, cfg_.max_y()) - cfg_.origin_y;
    CellCoord c;
    c.cx = static_cast<std::int32_t>(std::floor(x / cfg_.cell_size));
    c.cy = static_cast<std::int32_t>(std::floor(y / cfg_.cell_size));
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

    // Visit self's cell + a neighbour block sized to the LEAVE radius: a candidate
    // up to leave_radius away can be at most ceil(leave / cell) cells off in either
    // axis, so a (2k+1)×(2k+1) block is exactly sufficient. At production geometry
    // the interest radius exceeds one cell (R=90 m > 66.625 m cell → k=2), so this
    // is NOT a fixed 3×3 — sizing to the radius is what keeps a far-but-in-range
    // partner from hiding two cells away (#559). A full-map scan is unnecessary.
    const std::int32_t k =
        static_cast<std::int32_t>(std::ceil(cfg_.leave_radius / cfg_.cell_size));

    for (std::int32_t dy = -k; dy <= k; ++dy) {
        for (std::int32_t dx = -k; dx <= k; ++dx) {
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
                    if (dist <= cfg_.leave_radius) out.insert(cand);
                } else {
                    if (dist <= cfg_.enter_radius) out.insert(cand);
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
        static_cast<std::int32_t>(std::ceil(radius / cfg_.cell_size));

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
