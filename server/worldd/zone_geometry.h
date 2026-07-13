// SPDX-License-Identifier: Apache-2.0
//
// worldd — SINGLE-SOURCE zone geometry seam (issue #559; grown to real Zone-01 by
// #562; successor to #87).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §2.5 (Grid/AoI engine — 533 m
// grids × 8×8 cells, default interest R = 90 m, per-map config), §5.5 (movement
// map-bounds validation), the IF-6 chunk-manifest contract (schema/chunk/
// chunk-manifest.schema.yaml), and the #508 axis/origin lesson (client + server
// must share ONE authoritative zone origin). No GPL source consulted. See
// CONTRIBUTING.md.
//
// WHY THIS FILE EXISTS (#559): worldd's AoI grid used to bake the M0 bootstrap
// geometry — [0,128] play area, origin (0,0), 64 m cells — straight into
// aoi_grid.{h,cpp}. #559 parameterises the grid on a per-zone {origin, extent,
// cell, radii} (AoiGridConfig) and makes THIS header the ONE place the active
// zone's origin/extent live, so the AoI grid AND the movement validator derive
// their play area from a single source and can never silently diverge (the #508
// lesson: client chunk-streaming and server interest must agree on ONE origin).
//
// ─── WHERE THE ORIGIN/EXTENT COME FROM (#562) ────────────────────────────────
// The SINGLE authoritative source of Zone-01's origin/extent is the IF-6 chunk
// manifest (core:zone.zone01 → zone01.chunks.json), emitted by mcc (#22 Story 0 /
// #553) — the SAME artifact the client streamer reads, so client interest and
// server interest agree on ONE origin. The manifest fields below are transcribed
// here (kManifest*), the play area is derived from them (kZoneOrigin*/kZoneExtent*),
// and a static_assert proves movement_constants.h §8 (the movement validator's
// map bounds) equals that derived play area — a compile-time guarantee that the
// two never diverge into a second origin (#508). Zone content (zone01.zone.yaml →
// zone.schema.yaml → 80_zone.sql `zone.chunk_manifest`) still POINTS at this
// manifest rather than carrying geometry inline; a later story plumbs the manifest
// through the DB / IF-4 loader so these constants become the loader's typed default
// instead of a transcription (the same M1 migration movement-spike.md §4 describes).
//
// ─── AXIS MAP (#508) ─────────────────────────────────────────────────────────
// worldd's horizontal ground plane is (x, y) with z = height (movement_constants
// §7); the manifest is Godot XZ (Y-up), so the manifest z axis maps to worldd's y.
// Zone-01's grid is square and origin-symmetric, so both worldd axes share one
// [kZoneMinXY, kZoneMaxXY] bound and this seam derives a single square play area.

#ifndef MERIDIAN_WORLDD_ZONE_GEOMETRY_H
#define MERIDIAN_WORLDD_ZONE_GEOMETRY_H

#include "aoi_grid.h"
#include "movement_constants.h"

namespace meridian::worldd {

// ─── The IF-6 chunk manifest fields (zone01.chunks.json), transcribed ────────
// The world→cell transform is cx = floor((x − origin.x)/chunk_size_m) (manifest
// schema §origin), so the inclusive grid [min_c, max_c] covers zone-local metres
// [min_c·chunk + origin, (max_c+1)·chunk + origin]. These name the manifest so the
// derivation below is auditable against the artifact byte-for-byte.
inline constexpr float kManifestOriginXZ  = -384.0f;  // manifest origin.x == origin.z
inline constexpr float kManifestChunkSize = 128.0f;   // manifest chunk_size_m
inline constexpr int   kManifestGridMin   = -1;       // manifest grid.min_cx == min_cz
inline constexpr int   kManifestGridMax   =  1;       // manifest grid.max_cx == max_cz

// The active zone's horizontal play-area geometry, DERIVED from the manifest grid.
// origin = min corner; extent = grid span in metres. These are the SINGLE
// authoritative origin/extent the AoI grid is built on.
inline constexpr float kZoneOriginX = kManifestGridMin * kManifestChunkSize + kManifestOriginXZ;  // -512
inline constexpr float kZoneOriginY = kZoneOriginX;                                               // square, origin-symmetric
inline constexpr float kZoneExtentX = (kManifestGridMax - kManifestGridMin + 1) * kManifestChunkSize;  // 384
inline constexpr float kZoneExtentY = kZoneExtentX;

// ONE-ORIGIN cross-check (#508): the movement validator's map bounds (movement_
// constants §8) MUST equal the manifest-derived play area. If either side drifts,
// the server would hold two divergent origins — this fails the build instead.
static_assert(kZoneOriginX == movement::kZoneMinXY,
              "#508: AoI grid origin must equal the movement map-bounds min (one origin)");
static_assert(kZoneOriginX + kZoneExtentX == movement::kZoneMaxXY,
              "#508: AoI grid max (origin+extent) must equal the movement map-bounds max (one origin)");
static_assert(kZoneOriginY == movement::kZoneMinXY &&
                  kZoneOriginY + kZoneExtentY == movement::kZoneMaxXY,
              "#508: AoI grid must share the movement map bounds on the y axis too");

// The AoI grid config for the ACTIVE zone: the single-source manifest origin/extent
// above, the SAD §2.5 production cell size (~66 m) AND the production interest radii
// (enter R = 90 m / leave 100 m). #562 grew the play area past the old 128 m
// bootstrap square, so the production radii finally fit — no bootstrap down-scaling.
inline AoiGridConfig active_zone_aoi_config() {
    return production_aoi_config(kZoneOriginX, kZoneOriginY, kZoneExtentX, kZoneExtentY);
}

// The production interest radii must fit inside the play area so a session can
// actually move out of another's leave radius within the real extent (the #562
// verify: "a session can move out of interest range within the real extent").
static_assert(kAoiLeaveRadiusM < kZoneExtentX && kAoiLeaveRadiusM < kZoneExtentY,
              "production leave radius must fit inside the Zone-01 play area");

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_ZONE_GEOMETRY_H
