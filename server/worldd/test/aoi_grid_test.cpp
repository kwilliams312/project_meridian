// SPDX-License-Identifier: Apache-2.0
//
// worldd — Grid / AoI engine UNIT TEST (issue #87; parameterised on real zone
// geometry by #559). The core of the IT-M0 "two clients see each other move"
// capstone, now proving the grid works at an ARBITRARY zone origin/extent.
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 (Grid/AoI engine,
// cell-visitor "notify observers within R of P", 533 m grids × 8×8 cells ~66 m,
// default interest R = 90 m), §8.3 IT-M0 row (basic visitors), decision D-19
// (flat bootstrap map), aoi_grid.h + zone_geometry.h. No GPL source consulted
// (CONTRIBUTING).
//
// PURE / DB-FREE / SOCKET-FREE: exercises the AoiGrid spatial index + the
// interest-set query (enter/leave/hysteresis, membership) directly. It runs in
// the PLAIN server ctest (build.yml `server` job), no MariaDB service — the
// "pure grid+interest unit test so the core runs in the plain server ctest" ask
// of #87. The DB-backed two-client relay proof lives in world_relay_test.cpp
// (env-guarded, worldd-session CI job).
//
// What it proves:
//   A. MEMBERSHIP: two sessions within the enter radius see each other; two far
//      apart do not.
//   B. ENTER: a session moving from far to inside the enter radius newly enters
//      the observer's interest set.
//   C. LEAVE: a session moving from inside to beyond the leave radius leaves the
//      observer's interest set.
//   D. HYSTERESIS: a session parked in the band (enter < d ≤ leave) does NOT
//      thrash — it stays IN if it was in, stays OUT if it was out. Sub-metre
//      jitter across the enter line does not flip an already-out session in, nor
//      an already-in session out, until it fully crosses the band.
//   E. CELL INDEXING: sessions in the same and neighbouring cells are found; the
//      grid re-buckets a session when it crosses a cell boundary; remove() drops
//      it from all queries.
//   F. SELF: a session is never in its own interest set.
//   G. WITHIN_RADIUS: the one-shot chat say/yell visitor (no hysteresis).
//   H. NON-(0,0) ORIGIN + PRODUCTION R=90 (#559): at a shifted, negative-origin
//      zone with the SAD §2.5 production tuning (cell ~66 m, enter R=90 m), cell
//      indexing is correct relative to the origin, an entity within R=90 m ENTERS,
//      and the enter/leave hysteresis band still holds — with the interest radius
//      exceeding one cell (so the neighbour block is larger than 3×3).
//   I. REGRESSION — NOT HARDCODED TO [0,128]/(0,0) (#559): a grid whose origin is
//      not (0,0) indexes a world point OUTSIDE the old [0,128] bootstrap square to
//      the correct cell (the old hardcode would have clamped it into [0,128]).
//
// Sections A–G run on a COMPACT hysteresis/indexing-mechanics fixture (boot_cfg —
// origin (0,0), 128 m extent, ~66 m cells, enter 40 / leave 50 m). The ACTIVE
// zone config (zone_geometry.h: active_zone_aoi_config) is the real Zone-01 play
// area at PRODUCTION R=90/100 after #562 — verified by its own section and, at
// production scale + arbitrary origins, by sections H–I.

#include "aoi_grid.h"
#include "movement_constants.h"
#include "movement_validation.h"  // Position
#include "zone_geometry.h"        // active_zone_aoi_config() — the #559 zone seam

#include <cstdint>
#include <cstdio>
#include <unordered_set>

using namespace meridian::worldd;
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

bool has(const std::unordered_set<AoiId>& s, AoiId id) { return s.find(id) != s.end(); }

const std::unordered_set<AoiId> kEmpty;

// A COMPACT hysteresis/indexing-MECHANICS fixture sections A–G exercise: origin
// (0,0), 128 m extent, ~66 m cells, enter 40 / leave 50 m. This is deliberately
// small (radii that fit a 128 m square) so the enter/leave/hysteresis/cell-seam
// mechanics are easy to read; it is config-agnostic (the mechanics are the same at
// any scale). #562 flipped the ACTIVE zone (active_zone_aoi_config) to the real
// Zone-01 extent + PRODUCTION R=90/100 — verified by its own section below and by
// the production-scale sections H–I — so A–G no longer read the active config
// directly (their compact radii no longer fit the real extent).
AoiGridConfig boot_cfg() {
    AoiGridConfig cfg = production_aoi_config(0.0f, 0.0f, 128.0f, 128.0f);
    cfg.enter_radius = 40.0f;   // compact enter radius (fits the 128 m fixture)
    cfg.leave_radius = 50.0f;   // compact leave radius (> enter → hysteresis band)
    return cfg;
}

}  // namespace

int main() {
    std::printf("worldd Grid/AoI engine unit test (IT-M0 AoI core #87; zone geometry #559)\n");

    // Two guids used throughout. Sections A–G run on the compact mechanics fixture
    // boot_cfg() (origin (0,0), 128 m, centre (64,64)); the ACTIVE Zone-01 config is
    // verified separately just below and at production scale in H–I.
    const AoiId A = 1001;
    const AoiId B = 2002;
    const AoiId C = 3003;

    // ===== ACTIVE ZONE CONFIG — the #562 production flip =====================
    // active_zone_aoi_config() must now be the real Zone-01 play area (manifest
    // origin/extent, movement_constants §8 bounds) at PRODUCTION interest radii
    // (SAD §2.5 R=90 enter / 100 leave, ~66 m cells) — NOT the old bootstrap
    // 40/50-over-[0,128]. This pins the flip the story asked for.
    {
        AoiGridConfig z = active_zone_aoi_config();
        check("ACTIVE: origin is the Zone-01 min corner (-512)",
              z.origin_x == -512.0f && z.origin_y == -512.0f);
        check("ACTIVE: extent is the 3-chunk manifest grid (384 m)",
              z.extent_x == 384.0f && z.extent_y == 384.0f);
        check("ACTIVE: max corner is -128 (origin+extent)",
              z.max_x() == -128.0f && z.max_y() == -128.0f);
        check("ACTIVE: PRODUCTION enter radius R=90 (SAD §2.5)",
              z.enter_radius == kAoiEnterRadiusM && z.enter_radius == 90.0f);
        check("ACTIVE: PRODUCTION leave radius 100 (hysteresis band)",
              z.leave_radius == kAoiLeaveRadiusM && z.leave_radius == 100.0f);
        check("ACTIVE: production ~66 m cell size", z.cell_size == kAoiCellSizeM);

        // A session can move OUT of another's interest range within the real extent
        // (the #562 verify): two sessions 120 m apart (> 100 m leave) do NOT see
        // each other, and both fit inside [-512,-128]. -320 is the play-area centre.
        AoiGrid g(z);
        g.upsert(A, at(-320.0f, -320.0f));
        g.upsert(B, at(-320.0f, -200.0f));  // 120 m from A > 100 m leave radius
        check("ACTIVE: 120 m apart within the real extent — A does NOT see B",
              !has(g.interest_set(A, kEmpty), B));
        // Brought within R=90 (80 m apart), B ENTERS — production interest works.
        g.upsert(B, at(-320.0f, -240.0f));  // 80 m from A ≤ 90 enter
        check("ACTIVE: brought within R=90 on the real zone — B ENTERS",
              has(g.interest_set(A, kEmpty), B));
    }

    // ===== A. MEMBERSHIP =====================================================
    {
        AoiGrid g(boot_cfg());
        // A and B 10 m apart (well inside the 40 m enter radius) → mutually seen.
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(74.0f, 64.0f));
        auto a_sees = g.interest_set(A, kEmpty);
        auto b_sees = g.interest_set(B, kEmpty);
        check("A: 10 m apart — A sees B", has(a_sees, B));
        check("A: 10 m apart — B sees A", has(b_sees, A));
        check("A: interest set has exactly one member", a_sees.size() == 1);

        // Move B far away (60 m from A, beyond the 50 m leave radius) → neither sees.
        g.upsert(B, at(64.0f, 4.0f));  // dy = 60 m > 50 m leave radius
        auto a_sees2 = g.interest_set(A, kEmpty);
        check("A: 60 m apart — A does NOT see B", !has(a_sees2, B));
        check("A: 60 m apart — A's interest set empty", a_sees2.empty());
    }

    // ===== B. ENTER (far → inside enter radius) =============================
    {
        AoiGrid g(boot_cfg());
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(64.0f, 20.0f));  // 44 m away > 40 m enter radius
        auto prev = g.interest_set(A, kEmpty);
        check("B: 44 m away — B not yet visible to A", !has(prev, B));

        // B walks in to 30 m (inside enter radius) → newly ENTERS.
        g.upsert(B, at(64.0f, 34.0f));  // 30 m away
        auto now = g.interest_set(A, prev);
        check("B: B walked to 30 m — B ENTERS A's interest set", has(now, B));
    }

    // ===== C. LEAVE (inside → beyond leave radius) ==========================
    {
        AoiGrid g(boot_cfg());
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(64.0f, 54.0f));  // 10 m away — visible
        auto prev = g.interest_set(A, kEmpty);
        check("C: 10 m away — B visible to A", has(prev, B));

        // B walks out to 55 m (beyond the 50 m leave radius) → LEAVES.
        g.upsert(B, at(64.0f, 9.0f));  // 55 m away
        auto now = g.interest_set(A, prev);
        check("C: B walked to 55 m — B LEAVES A's interest set", !has(now, B));
    }

    // ===== D. HYSTERESIS (the anti-thrash band) =============================
    {
        // Place B in the band: enter(40) < d ≤ leave(50). At d = 45 m the
        // membership is STICKY — it is whatever it already was.
        AoiGrid g(boot_cfg());
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(64.0f, 19.0f));  // 45 m away — in the band

        // Starting OUT (previous empty): B is beyond the enter radius, so it does
        // NOT enter just because it is inside the leave radius.
        auto out_prev = g.interest_set(A, kEmpty);
        check("D: band (45 m), was OUT — stays OUT (no spurious enter)",
              !has(out_prev, B));

        // Starting IN (previous = {B}): B is inside the leave radius, so it does
        // NOT leave — sticky.
        std::unordered_set<AoiId> in_prev = {B};
        auto stay_in = g.interest_set(A, in_prev);
        check("D: band (45 m), was IN — stays IN (no spurious leave)",
              has(stay_in, B));

        // No-thrash proof: jitter B by a fraction of a metre around the enter
        // line (40 m) while it was already OUT. It must not flip in until it
        // clears the enter radius, and once in, must not flip out until past the
        // leave radius. Simulate a boundary hover: 40.5 → 39.5 → 40.5 while OUT.
        std::unordered_set<AoiId> prev = {};  // starts OUT
        g.upsert(B, at(64.0f, 23.5f));  // 40.5 m — just outside enter, OUT stays OUT
        prev = g.interest_set(A, prev);
        check("D: 40.5 m hover, was OUT — still OUT", !has(prev, B));
        g.upsert(B, at(64.0f, 24.5f));  // 39.5 m — just inside enter → ENTERS
        prev = g.interest_set(A, prev);
        check("D: 39.5 m — crosses enter line → ENTERS", has(prev, B));
        g.upsert(B, at(64.0f, 23.5f));  // 40.5 m — back in band, was IN → sticky IN
        prev = g.interest_set(A, prev);
        check("D: 40.5 m hover, was IN — sticky IN (no thrash)", has(prev, B));
    }

    // ===== E. CELL INDEXING =================================================
    {
        AoiGrid g(boot_cfg());
        // With ~66 m cells over [0,128], (60,10) is cell (0,0); (70,10) is cell
        // (1,0) — neighbours. A query must still find a partner in a neighbour
        // cell within radius.
        g.upsert(A, at(60.0f, 10.0f));   // cell (0,0)
        g.upsert(B, at(70.0f, 10.0f));   // cell (1,0), 10 m from A across the seam
        check("E: A cell is (0,0)", g.cell_of(at(60.0f, 10.0f)) == CellCoord{0, 0});
        check("E: B cell is (1,0)", g.cell_of(at(70.0f, 10.0f)) == CellCoord{1, 0});
        auto a_sees = g.interest_set(A, kEmpty);
        check("E: neighbour-cell partner found (A sees B across the cell seam)",
              has(a_sees, B));

        // Move B into A's cell and confirm re-bucketing keeps it found.
        g.upsert(B, at(64.0f, 10.0f));  // now cell (0,0), still ~4 m from A
        check("E: after move, tracked count still 2", g.size() == 2);
        auto a_sees2 = g.interest_set(A, kEmpty);
        check("E: re-bucketed partner still found", has(a_sees2, B));

        // remove() drops B from every query.
        g.remove(B);
        check("E: after remove, count 1", g.size() == 1);
        check("E: after remove, A no longer contains B", !g.contains(B));
        auto a_sees3 = g.interest_set(A, kEmpty);
        check("E: after remove, A's interest set empty", a_sees3.empty());
    }

    // ===== F. SELF ==========================================================
    {
        AoiGrid g(boot_cfg());
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(64.0f, 64.0f));  // same spot as A
        g.upsert(C, at(64.0f, 64.0f));
        auto a_sees = g.interest_set(A, kEmpty);
        check("F: A never sees itself", !has(a_sees, A));
        check("F: A sees the two co-located others (B and C)",
              has(a_sees, B) && has(a_sees, C) && a_sees.size() == 2);
    }

    // ===== G. WITHIN_RADIUS (chat say/yell visitor; SOC-01 #367) ============
    // The one-shot radius query (no hysteresis): a candidate is in iff its
    // distance ≤ the requested radius, whatever that radius is (a tight say, a
    // wide yell), and self is never included.
    {
        AoiGrid g(boot_cfg());
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(70.0f, 64.0f));   //  6 m from A
        g.upsert(C, at(64.0f, 94.0f));   // 30 m from A

        // Say radius 25 m: B (6 m) is in, C (30 m) is out.
        auto say = g.within_radius(A, kChatSayRadiusM);
        check("G: say radius — near B in", has(say, B));
        check("G: say radius — far C out", !has(say, C));
        check("G: say radius — self excluded", !has(say, A));
        check("G: say radius — exactly one in range", say.size() == 1);

        // Yell radius 90 m (spans > 1 cell): both B and C are in — within_radius
        // sizes its scan block to the radius (k = ceil(90 / ~66) = 2, a 5×5 block).
        auto yell = g.within_radius(A, kChatYellRadiusM);
        check("G: yell radius — near B in", has(yell, B));
        check("G: yell radius — far C now in (wider than say)", has(yell, C));
        check("G: yell radius — both in range", yell.size() == 2);

        // A zero/negative radius is a no-op empty set; an untracked id too.
        check("G: zero radius — empty", g.within_radius(A, 0.0f).empty());
        check("G: untracked self — empty", g.within_radius(9999, kChatYellRadiusM).empty());
    }

    // ===== H. NON-(0,0) ORIGIN + PRODUCTION R=90 (#559) =====================
    // A production-tuned zone at a SHIFTED, NEGATIVE origin (like the real Zone-01
    // POIs, which run negative): origin (-256,-256), 512 m extent, SAD §2.5 cell
    // ~66 m, enter R=90 m / leave 100 m. The interest radius EXCEEDS one cell, so
    // the neighbour block is larger than 3×3 (k = ceil(100 / ~66) = 2).
    {
        const float ox = -256.0f, oy = -256.0f;
        AoiGridConfig pcfg = production_aoi_config(ox, oy, /*extent=*/512.0f, 512.0f);
        check("H: production config uses SAD R=90 enter radius",
              pcfg.enter_radius == kAoiEnterRadiusM && kAoiEnterRadiusM == 90.0f);
        check("H: production config uses ~66 m cell", pcfg.cell_size == kAoiCellSizeM);
        AoiGrid g(pcfg);

        // Cell indexing is RELATIVE TO THE ORIGIN: the origin corner is cell (0,0),
        // and a point one-and-a-bit cells in is cell (1,0). Under the old
        // [0,128]/(0,0) hardcode these world points would have clamped to (0,0).
        check("H: origin corner indexes to cell (0,0)",
              g.cell_of(at(ox, oy)) == CellCoord{0, 0});
        check("H: one cell in from origin → cell (1,0)",
              g.cell_of(at(ox + kAoiCellSizeM + 1.0f, oy)) == CellCoord{1, 0});

        // An entity within R=90 m of a session ENTERS its interest set; one just
        // outside R does not (fresh, previous empty). A is at the zone centre (0,0).
        g.upsert(A, at(0.0f, 0.0f));
        g.upsert(B, at(0.0f, -80.0f));   // 80 m from A ≤ 90 enter
        g.upsert(C, at(0.0f, -95.0f));   // 95 m from A > 90 enter (fresh)
        auto a_sees = g.interest_set(A, kEmpty);
        check("H: entity 80 m away (≤ R=90) ENTERS A's interest set", has(a_sees, B));
        check("H: entity 95 m away (> R=90) does NOT enter fresh", !has(a_sees, C));

        // ENTER across the R=90 line: C walks from 95 m (out) to 85 m (in) → enters.
        g.upsert(C, at(0.0f, -85.0f));   // 85 m from A ≤ 90
        auto now = g.interest_set(A, a_sees);
        check("H: C walks inside R=90 → ENTERS", has(now, C));

        // HYSTERESIS at production radii: park a session in the band 90 < d ≤ 100.
        // At 95 m it is sticky — OUT stays OUT, IN stays IN.
        g.upsert(B, at(0.0f, -95.0f));   // 95 m — in the 90..100 band
        check("H: band (95 m), was OUT — stays OUT",
              !has(g.interest_set(A, kEmpty), B));
        check("H: band (95 m), was IN — stays IN",
              has(g.interest_set(A, std::unordered_set<AoiId>{B}), B));

        // LEAVE past the 100 m leave radius: B to 105 m → leaves even if it was IN.
        g.upsert(B, at(0.0f, -105.0f));  // 105 m > 100 leave radius (still in bounds)
        check("H: B beyond 100 m leave radius → LEAVES",
              !has(g.interest_set(A, std::unordered_set<AoiId>{B}), B));
    }

    // ===== I. REGRESSION — grid is NOT hardcoded to [0,128]/(0,0) (#559) =====
    // A grid at a POSITIVE, non-zero origin must index a world point that lies
    // OUTSIDE the old [0,128] bootstrap square to the correct cell. Under the old
    // hardcode, cell_of clamped every axis into [0,128] and subtracted origin 0, so
    // a point at (200,200) would clamp to (128,128) → cell (2,2) with 64 m cells.
    // With the parameterised grid (origin (200,200)) it is the origin corner → (0,0).
    {
        const float ox = 200.0f, oy = 200.0f;
        AoiGrid g(production_aoi_config(ox, oy, /*extent=*/200.0f, 200.0f));
        check("I: point at the (200,200) origin → cell (0,0), NOT clamped to [0,128]",
              g.cell_of(at(200.0f, 200.0f)) == CellCoord{0, 0});
        check("I: point deep in the shifted zone indexes past cell (0,0)",
              g.cell_of(at(333.0f, 200.0f)) == CellCoord{1, 0});

        // Two entities living entirely outside the old [0,128] square still track at
        // their real positions and see each other (they would have collapsed onto
        // the clamped (128,128) corner under the old hardcode).
        g.upsert(A, at(300.0f, 300.0f));
        g.upsert(B, at(308.0f, 300.0f));  // 8 m from A ≤ 90 enter
        check("I: entities outside old [0,128] are tracked and mutually visible",
              has(g.interest_set(A, kEmpty), B));
        check("I: their positions are NOT clamped into [0,128]",
              g.position_of(A).x == 300.0f && g.position_of(B).x == 308.0f);
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD AOI GRID TESTS PASSED\n"
                            : "\n%d WORLDD AOI GRID TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
