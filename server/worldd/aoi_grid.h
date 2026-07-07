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
// ─── THE GRID (SAD §2.5 / §3.2, D-19 flat bootstrap) ─────────────────────────
// The SAD's production geometry is 533 m grids × 8×8 cells (~66 m) with a
// default interest radius R = 90 m. At M0 (D-19) worldd runs on the flat 128 m
// bootstrap chunk (movement_constants.h kZoneMinXY..kZoneMaxXY = [0,128] m), so a
// single uniform cell grid over that square is sufficient — no grid activation /
// deactivation is needed for one small map. We partition [0,128]² into square
// cells of side kAoiCellSizeM. A session's cell index is derived from its
// authoritative (x, y). An interest query for session S visits S's cell plus its
// neighbour ring (a 3×3 block of cells when the cell side ≥ R, which it is at
// M0) and keeps the sessions whose euclidean horizontal distance from S is
// within the enter/leave radius. Positions are clamped into the play area before
// indexing, so an out-of-bounds authoritative position (which the #86 validator
// already prevents) can never index outside the grid.
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
// AoI tuning (v0, flat bootstrap map). Traceable to SAD §2.5 defaults, scaled
// for the M0 128 m play area.
// ---------------------------------------------------------------------------

// Cell side (metres). The SAD's production cell is ~66 m; on the 128 m bootstrap
// chunk we use 64 m cells so the whole play area is a small 2×2 grid and one 3×3
// neighbour block always covers a session's full leave-radius reach (64 m ≥ the
// leave radius below, so no interest partner can hide two cells away). Uniform,
// axis-aligned, origin at (kZoneMinXY, kZoneMinXY).
inline constexpr float kAoiCellSizeM = 64.0f;

// Enter/leave radii (metres) — the Schmitt-trigger band (see file header). The
// SAD default interest radius is R = 90 m; at M0 we pick an enter radius
// comfortably inside a 64 m cell's 3×3 reach and a leave radius a band wider so
// membership is sticky between them. These are the "AoI radius" the story asks
// for; the small enter<leave gap is the hysteresis.
inline constexpr float kAoiEnterRadiusM = 40.0f;  // come within 40 m to become visible
inline constexpr float kAoiLeaveRadiusM = 50.0f;  // leave only past 50 m (10 m sticky band)

static_assert(kAoiLeaveRadiusM > kAoiEnterRadiusM,
              "AoI hysteresis requires leave radius > enter radius");
static_assert(kAoiLeaveRadiusM <= kAoiCellSizeM,
              "leave radius must fit inside a cell's 3x3 neighbour reach so no "
              "in-range partner is missed by the neighbour-block query");

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
    AoiGrid() = default;

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
    // Only cells in `self`'s 3×3 neighbour block are scanned (the leave radius
    // fits one cell, so no in-range partner is outside that block). `self` is
    // never in its own interest set. If `self` is not tracked, the result is
    // empty.
    std::unordered_set<AoiId> interest_set(
        AoiId self, const std::unordered_set<AoiId>& previous) const;

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

    std::unordered_map<AoiId, Position> positions_;
    std::unordered_map<AoiId, CellCoord> cell_by_id_;
    std::unordered_map<CellKey, std::unordered_set<AoiId>, CellKeyHash> ids_by_cell_;
};

// Horizontal (x, y) euclidean distance between two positions (z is not an AoI
// axis at M0 — the flat bootstrap map is a single plane, D-19).
float horizontal_distance(const Position& a, const Position& b);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_AOI_GRID_H
