// SPDX-License-Identifier: Apache-2.0
//
// worldd — Grid / AoI engine v0 (issue #87; the IT-M0 "two clients see each
// other move" capstone).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md only — §2.5 ("Grid / AoI
// engine — grids × cells; cell-visitor queries implement 'notify observers
// within R of P'"), §3.2 (the 128 m chunk / cell structure), §8.3 IT-M0 row
// ("Grid/AoI: basic visitors, activation" + "echo world state — movement + AoI
// relay only"), the "authoritative state → interest set → per-subscriber
// egress" flow, decision D-19 (M0 flat bootstrap map), and world.fbs
// (EntityEnter / EntityUpdate / EntityLeave / MovementState). No GPL source
// (CMaNGOS / TrinityCore or otherwise) was consulted. See CONTRIBUTING.md.
//
// WHAT THIS FILE IS: the PURE, DB-free, socket-free spatial core. It answers one
// question — "given every session's authoritative position, which OTHER sessions
// are within AoI radius R of session S?" — and it answers it with hysteresis so
// a session straddling a cell/radius boundary does not thrash EntityEnter /
// EntityLeave every tick. It has NO dependency on FlatBuffers, the DB, or a
// socket, so the interest-set logic runs in the plain `server` ctest with no
// MariaDB (the "pure grid+interest unit test" ask of #87). The relay that turns
// an interest-set DELTA into EntityEnter/Update/Leave frames and routes them
// through each subscriber's WorldSession lives in world_state.{h,cpp}, on top of
// this core.
//
// ─── THE GRID (SAD §2.5 / §3.2; #559 real zone geometry) ─────────────────────
// The SAD's production geometry is 533 m grids × 8×8 cells (66.625 m) with a
// default interest radius R = 90 m. The grid is PARAMETERISED on a per-zone
// AoiGridConfig — {origin (min corner), extent (size per axis), cell size, and
// the enter/leave interest radii} — instead of the M0 [0,128]/(0,0)/64 m bootstrap
// constants #87 baked in (#559). The single authoritative source of a zone's
// origin/extent is zone_geometry.h (which binds them to the best zone geometry
// worldd has, with a TODO to read the shared zone manifest — #22 Story 0, #508).
// We partition the [origin, origin+extent] square into cells of side cfg.cell_size.
// A session's cell index is derived from its authoritative (x, y) RELATIVE TO THE
// ORIGIN (so a non-(0,0) origin indexes correctly). An interest query for session
// S visits S's cell plus a neighbour block SIZED TO THE LEAVE RADIUS (a
// (2k+1)×(2k+1) block, k = ceil(leave / cell)) — so even when the interest radius
// exceeds one cell (as at the production R=90 m / 66.625 m cell) no in-range
// partner is missed. It keeps the sessions whose euclidean horizontal distance
// from S is within the enter/leave radius. Positions are clamped into the play
// area before indexing, so an out-of-bounds authoritative position (which the #86
// validator already prevents) can never index outside the grid.
//
// ─── HYSTERESIS (SAD §2.3 "population-band hysteresis" pattern applied to AoI) ──
// A single radius R would make a session hovering exactly at distance R flip
// in/out of another's interest set on tiny sub-metre jitter, emitting a storm of
// EntityEnter/EntityLeave. We use TWO radii (a classic Schmitt trigger):
//   • kAoiEnterRadiusM  — a session must come WITHIN this to newly ENTER an
//                         observer's interest set.
//   • kAoiLeaveRadiusM  — an already-visible session only LEAVES once it moves
//                         BEYOND this (kAoiLeaveRadiusM > kAoiEnterRadiusM).
// Between the two radii the membership is STICKY: whatever it already was, it
// stays. So a session must cross a band (not a line) to change state, which
// kills boundary thrash. `interest_set(...)` therefore takes the observer's
// PREVIOUS interest set to resolve the sticky band.

#ifndef MERIDIAN_WORLDD_AOI_GRID_H
#define MERIDIAN_WORLDD_AOI_GRID_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "movement_validation.h"  // Position

namespace meridian::worldd {

// A stable per-session identity in the AoI world. At M0 this is the mover's
// entity guid (the placeholder character guid from enter-world); the grid never
// interprets it beyond "a unique key", so any 64-bit id works.
using AoiId = std::uint64_t;

// ---------------------------------------------------------------------------
// AoI tuning — SAD §2.5 PRODUCTION defaults (#559). These are the values an
// AoiGridConfig carries unless a zone overrides them; they replace the M0 128 m
// bootstrap-scaled constants #87 used. Origin/extent are NOT here — they are a
// per-zone parameter (AoiGridConfig, sourced from zone_geometry.h).
// ---------------------------------------------------------------------------

// Cell side (metres). SAD §2.5 production geometry is 533 m grids × 8×8 cells, i.e.
// 533 / 8 = 66.625 m per cell. Uniform, axis-aligned; the grid ORIGIN is a per-zone
// parameter (AoiGridConfig::origin_*), never a hardcoded (0,0).
inline constexpr float kAoiCellSizeM = 66.625f;  // 533 m / 8 cells (SAD §2.5)

// Enter/leave radii (metres) — the Schmitt-trigger hysteresis band (see file
// header). SAD §2.5 default interest radius is R = 90 m; that is the ENTER radius.
// The LEAVE radius is a band wider so membership is sticky between them (SAD §2.3
// population-band hysteresis applied to AoI). The enter/leave radii can (and at
// production DO) exceed one cell, so interest_set sizes its neighbour scan block
// to the leave radius (k = ceil(leave / cell)) rather than assuming 3×3 — a
// partner up to a full leave-radius away is never missed.
inline constexpr float kAoiEnterRadiusM = 90.0f;   // SAD §2.5 default interest R
inline constexpr float kAoiLeaveRadiusM = 100.0f;  // 10 m sticky hysteresis band

static_assert(kAoiLeaveRadiusM > kAoiEnterRadiusM,
              "AoI hysteresis requires leave radius > enter radius");

// Chat say/yell radii (SOC-01 #367) — spatial tuning, so they live here with the
// AoI radii and feed within_radius(). say = a TIGHT local radius; yell = WIDER
// (the story's "say = local cell radius, yell = wider"). Traceable to SAD §2.5
// (default interest R = 90 m). Unlike the movement enter/leave band, these are
// one-shot (no hysteresis) and yell may exceed a single cell — within_radius sizes
// its scan block to the radius.
inline constexpr float kChatSayRadiusM  = 25.0f;   // /say — local
inline constexpr float kChatYellRadiusM = 90.0f;   // /yell — wider (SAD default R)
static_assert(kChatYellRadiusM > kChatSayRadiusM, "yell must reach wider than say");

// ---------------------------------------------------------------------------
// AoiGridConfig — the per-zone geometry the grid is parameterised on (#559).
// ---------------------------------------------------------------------------
// The grid no longer hardcodes the M0 [0,128]/(0,0)/64 m bootstrap. A zone supplies
// its play-area ORIGIN (min corner, zone-local metres) + EXTENT (size along each
// axis) + CELL SIZE + the enter/leave interest radii. origin + extent define the
// [origin, origin+extent] square positions are clamped into before cell indexing;
// cell_of derives the cell index relative to the origin. The single authoritative
// source of a zone's origin/extent is zone_geometry.h (which today binds them to
// the best zone geometry worldd has, with a TODO to read the shared zone manifest
// origin — #22 Story 0 / the #508 axis/origin lesson). Fields default to the SAD
// §2.5 production tuning; origin/extent default to a degenerate zero area and MUST
// be set by a factory (production_aoi_config) or an explicit zone config.
struct AoiGridConfig {
    float origin_x = 0.0f;       // play-area min x (zone-local metres)
    float origin_y = 0.0f;       // play-area min y
    float extent_x = 0.0f;       // play-area size along x (max_x = origin_x + extent_x)
    float extent_y = 0.0f;       // play-area size along y
    float cell_size = kAoiCellSizeM;
    float enter_radius = kAoiEnterRadiusM;
    float leave_radius = kAoiLeaveRadiusM;

    float max_x() const { return origin_x + extent_x; }
    float max_y() const { return origin_y + extent_y; }
};

// Assemble a production-tuned config (SAD §2.5: cell 66.625 m, enter R=90 m, leave
// 100 m) over the given zone play area [origin, origin+extent]. The ONE place the
// production tuning is bound to a concrete origin/extent: zone_geometry.h calls it
// for the active zone; the pure unit tests call it for arbitrary origins/extents.
inline AoiGridConfig production_aoi_config(float origin_x, float origin_y,
                                           float extent_x, float extent_y) {
    AoiGridConfig cfg;
    cfg.origin_x = origin_x;
    cfg.origin_y = origin_y;
    cfg.extent_x = extent_x;
    cfg.extent_y = extent_y;
    // cell_size / enter_radius / leave_radius keep their production defaults.
    return cfg;
}

// A discrete cell coordinate on the uniform grid.
struct CellCoord {
    std::int32_t cx = 0;
    std::int32_t cy = 0;
    bool operator==(const CellCoord& o) const { return cx == o.cx && cy == o.cy; }
};

// ---------------------------------------------------------------------------
// AoiGrid — the pure spatial index + interest-set query.
// ---------------------------------------------------------------------------
//
// Owned by the world thread (SAD §2.5/§6: game state is single-threaded). It is
// a plain in-memory structure: a map of id → authoritative position, plus a
// bucketing of ids by cell so a neighbour query does not scan every session.
// Not thread-safe by itself — the world-thread ownership (or an external lock,
// as world_state.cpp uses at M0) provides the serialization.
class AoiGrid {
public:
    // Construct on an explicit per-zone geometry (#559). There is NO default
    // geometry — a zone's origin/extent/cell/radii must be supplied so the grid can
    // never silently fall back to the old [0,128]/(0,0)/64 m hardcode. world_state
    // builds the active zone's config via zone_geometry.h; tests build arbitrary
    // ones via production_aoi_config.
    explicit AoiGrid(const AoiGridConfig& cfg) : cfg_(cfg) {}

    // The zone geometry this grid indexes on.
    const AoiGridConfig& config() const { return cfg_; }

    // Insert or move a session to `pos`. Re-buckets it into the correct cell.
    // Idempotent for an unchanged position. The position is clamped into the
    // play area for cell indexing (an out-of-bounds pos still tracks, at the
    // boundary cell) — the #86 validator already keeps authoritative positions
    // in-bounds, this is defence in depth.
    void upsert(AoiId id, const Position& pos);

    // Remove a session entirely (world-leave / disconnect). No-op if absent.
    void remove(AoiId id);

    // Whether `id` is currently tracked.
    bool contains(AoiId id) const;

    // The tracked authoritative position of `id` (origin if absent).
    Position position_of(AoiId id) const;

    // How many sessions are tracked.
    std::size_t size() const { return positions_.size(); }

    // The cell an (x, y) maps to (clamped into the play area).
    CellCoord cell_of(const Position& pos) const;

    // Compute the interest set of observer `self`: the set of OTHER tracked
    // session ids within AoI radius of `self`, WITH HYSTERESIS resolved against
    // `previous` (self's interest set from the last query). A candidate:
    //   • ENTERS the set if it is not in `previous` and its distance ≤ enter R.
    //   • STAYS in the set if it is in `previous` and its distance ≤ leave R.
    //   • is otherwise OUT.
    // The scan visits `self`'s neighbour block sized to the leave radius (a
    // (2k+1)×(2k+1) block, k = ceil(leave / cell)), so no in-range partner is
    // missed even when the interest radius exceeds one cell (as at production
    // R=90 m / 66.625 m cell). `self` is never in its own interest set. If `self`
    // is not tracked, the result is empty.
    std::unordered_set<AoiId> interest_set(
        AoiId self, const std::unordered_set<AoiId>& previous) const;

    // Compute the set of OTHER tracked session ids whose horizontal distance from
    // `self` is ≤ `radius` — a PURE, ONE-SHOT radius query with NO hysteresis (a
    // chat/say/yell "notify observers within R of P" visitor, SAD §2.5, distinct
    // from the movement interest set's sticky enter/leave band). `self` is never
    // in the result; an untracked `self` yields the empty set. Like interest_set,
    // this scans a (2k+1)×(2k+1) block sized to the requested radius (k =
    // ceil(radius / cell)), so an arbitrary radius — a wide yell, a small say — is
    // covered exactly. Used by the SOC-01 chat router (#367) for spatial say/yell.
    std::unordered_set<AoiId> within_radius(AoiId self, float radius) const;

private:
    // Bucket key for the by-cell index.
    struct CellKey {
        std::int32_t cx;
        std::int32_t cy;
        bool operator==(const CellKey& o) const { return cx == o.cx && cy == o.cy; }
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& k) const {
            // Cantor-ish mix of two int32s into a size_t bucket hash.
            std::uint64_t a = static_cast<std::uint32_t>(k.cx);
            std::uint64_t b = static_cast<std::uint32_t>(k.cy);
            return std::hash<std::uint64_t>{}((a << 32) ^ b);
        }
    };

    AoiGridConfig cfg_;  // the per-zone geometry (#559) — origin/extent/cell/radii
    std::unordered_map<AoiId, Position> positions_;
    std::unordered_map<AoiId, CellCoord> cell_by_id_;
    std::unordered_map<CellKey, std::unordered_set<AoiId>, CellKeyHash> ids_by_cell_;
};

// Horizontal (x, y) euclidean distance between two positions (z is not an AoI
// axis at M0 — the flat bootstrap map is a single plane, D-19).
float horizontal_distance(const Position& a, const Position& b);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_AOI_GRID_H
