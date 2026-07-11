// SPDX-License-Identifier: Apache-2.0
//
// worldd — SINGLE-SOURCE zone geometry seam (issue #559; successor to #87).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §2.5 (Grid/AoI engine — 533 m
// grids × 8×8 cells, default interest R = 90 m, per-map config), §5.5 (movement
// map-bounds validation), decision D-19 (M0 flat bootstrap map), and the #508
// axis/origin lesson (client + server must share ONE authoritative zone origin).
// No GPL source consulted. See CONTRIBUTING.md.
//
// WHY THIS FILE EXISTS (#559): worldd's AoI grid used to bake the M0 bootstrap
// geometry — [0,128] play area, origin (0,0), 64 m cells — straight into
// aoi_grid.{h,cpp}. #559 parameterises the grid on a per-zone {origin, extent,
// cell, radii} (AoiGridConfig) and makes THIS header the ONE place the active
// zone's origin/extent live, so the AoI grid AND the movement validator derive
// their play area from a single source and can never silently diverge (the #508
// lesson: client chunk-streaming and server interest must agree on ONE origin).
//
// ─── WHERE THE ORIGIN/EXTENT COME FROM ───────────────────────────────────────
// The zone content worldd loads (content/core/zones/zone01.zone.yaml →
// schema/content/zone.schema.yaml → schema/sql/world/80_zone.sql `zone` table)
// does NOT yet carry a zone origin or extent — geometry lives in Forge-exported
// chunk data whose manifest is RESERVED until the A-08 chunk-format contract
// (zone.chunk_manifest = null). So the best zone geometry worldd has today is the
// movement validator's bootstrap play area (movement_constants.h §8, D-19): the
// [kZoneMinXY, kZoneMaxXY] square. We bind the AoI grid to exactly that square
// here, so interest and movement bounds stay consistent by construction.
//
// TODO(#559 / #22 Story 0): source origin + extent from the SHARED zone manifest —
// the same authoritative origin the client's chunk-streaming reads — once #22
// Story 0 plumbs a zone origin/extent into the world DB / IF-4 artifact set. When
// that lands, BOTH this seam and movement_constants.h §8 read the manifest origin;
// do NOT introduce a second, divergent origin in the meantime.
//
// TODO(#559): the bootstrap play area is only kZoneMaxXY (= 128 m) across —
// SMALLER than the SAD §2.5 production interest radius (R = 90 m). Until the zone
// extent grows to real Zone-01 geometry, the ACTIVE interest radii are scaled to
// fit the bootstrap square (kBootstrap*RadiusM below) so AoI enter/leave still has
// a meaningful hysteresis band inside [0,128]. Growing the extent requires the
// movement map-bounds ([0,128]) to grow too, and those bounds are pinned by the
// CROSS-TRACK golden fixture (movement_fixture.h `out_of_bounds`, mirrored on the
// client doctest track) — so the extent/bounds growth is a coordinated cross-track
// change (its own story), NOT this one. The production radii (kAoiEnterRadiusM =
// 90 m / kAoiLeaveRadiusM = 100 m) are the AoiGridConfig defaults and are proven
// by the pure #87 grid unit tests at a real (large) extent.

#ifndef MERIDIAN_WORLDD_ZONE_GEOMETRY_H
#define MERIDIAN_WORLDD_ZONE_GEOMETRY_H

#include "aoi_grid.h"
#include "movement_constants.h"

namespace meridian::worldd {

// The active zone's horizontal play-area geometry — the SINGLE authoritative
// source, bound to the movement validator's bootstrap bounds (movement_constants.h
// §8) so the AoI grid and map-bounds validation never diverge. See the file-header
// TODOs for the shared-manifest origin (#22 Story 0) that will replace these.
inline constexpr float kZoneOriginX = movement::kZoneMinXY;
inline constexpr float kZoneOriginY = movement::kZoneMinXY;
inline constexpr float kZoneExtentX = movement::kZoneMaxXY - movement::kZoneMinXY;
inline constexpr float kZoneExtentY = movement::kZoneMaxXY - movement::kZoneMinXY;

// M0/M1 BOOTSTRAP interest radii — scaled to fit the 128 m bootstrap square (see
// the file-header TODO). These stand in for the production R = 90 m enter radius
// (kAoiEnterRadiusM) only until the zone extent grows past ~2·R. Enter < leave
// keeps the hysteresis band; both fit within the 128 m play area so a session can
// actually move out of another's leave radius inside the bootstrap map.
inline constexpr float kBootstrapEnterRadiusM = 40.0f;  // come within 40 m to enter
inline constexpr float kBootstrapLeaveRadiusM = 50.0f;  // leave only past 50 m

static_assert(kBootstrapLeaveRadiusM > kBootstrapEnterRadiusM,
              "bootstrap AoI hysteresis requires leave radius > enter radius");
static_assert(kBootstrapLeaveRadiusM < kZoneExtentX &&
                  kBootstrapLeaveRadiusM < kZoneExtentY,
              "bootstrap leave radius must fit inside the play area so a session "
              "can leave another's interest set inside the bootstrap map");

// The AoI grid config for the ACTIVE zone: the single-source origin/extent above,
// the SAD §2.5 production cell size (~66 m, from production_aoi_config), and the
// bootstrap-scaled interest radii above. When the zone extent grows to production
// geometry, drop the two radius overrides so the production R = 90 m defaults apply.
inline AoiGridConfig active_zone_aoi_config() {
    AoiGridConfig cfg = production_aoi_config(kZoneOriginX, kZoneOriginY,
                                              kZoneExtentX, kZoneExtentY);
    cfg.enter_radius = kBootstrapEnterRadiusM;
    cfg.leave_radius = kBootstrapLeaveRadiusM;
    return cfg;
}

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_ZONE_GEOMETRY_H
