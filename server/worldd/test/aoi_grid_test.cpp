// SPDX-License-Identifier: Apache-2.0
//
// worldd — Grid / AoI engine v0 UNIT TEST (issue #87, the core of the IT-M0
// "two clients see each other move" capstone).
//
// CLEAN-ROOM: written from docs/sad/server-sad.md §2.5 (Grid/AoI engine,
// cell-visitor "notify observers within R of P"), §8.3 IT-M0 row (basic
// visitors), decision D-19 (flat bootstrap map), and aoi_grid.h. No GPL source
// consulted (CONTRIBUTING).
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

#include "aoi_grid.h"
#include "movement_constants.h"
#include "movement_validation.h"  // Position

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

}  // namespace

int main() {
    std::printf("worldd Grid/AoI engine v0 unit test (IT-M0 AoI core, #87)\n");

    // Two guids used throughout. The play-area centre is (64, 64) on the 128 m
    // bootstrap chunk (kZoneMinXY..kZoneMaxXY = [0,128]).
    const AoiId A = 1001;
    const AoiId B = 2002;
    const AoiId C = 3003;

    // ===== A. MEMBERSHIP =====================================================
    {
        AoiGrid g;
        // A and B 10 m apart (well inside the 40 m enter radius) → mutually seen.
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(74.0f, 64.0f));
        auto a_sees = g.interest_set(A, kEmpty);
        auto b_sees = g.interest_set(B, kEmpty);
        check("A: 10 m apart — A sees B", has(a_sees, B));
        check("A: 10 m apart — B sees A", has(b_sees, A));
        check("A: interest set has exactly one member", a_sees.size() == 1);

        // Move B far away (100 m from A, beyond leave radius) → neither sees.
        g.upsert(B, at(64.0f, 4.0f));  // dy = 60 m > 50 m leave radius
        auto a_sees2 = g.interest_set(A, kEmpty);
        check("A: 60 m apart — A does NOT see B", !has(a_sees2, B));
        check("A: 60 m apart — A's interest set empty", a_sees2.empty());
    }

    // ===== B. ENTER (far → inside enter radius) =============================
    {
        AoiGrid g;
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
        AoiGrid g;
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
        AoiGrid g;
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
        AoiGrid g;
        // With 64 m cells over [0,128], (10,10) is cell (0,0); (70,10) is cell
        // (1,0) — neighbours. A query must still find a partner in a neighbour
        // cell within radius.
        g.upsert(A, at(60.0f, 10.0f));   // cell (0,0), near the x=64 boundary
        g.upsert(B, at(70.0f, 10.0f));   // cell (1,0), 10 m from A across the seam
        check("E: A cell is (0,0)", g.cell_of(at(60.0f, 10.0f)) == CellCoord{0, 0});
        check("E: B cell is (1,0)", g.cell_of(at(70.0f, 10.0f)) == CellCoord{1, 0});
        auto a_sees = g.interest_set(A, kEmpty);
        check("E: neighbour-cell partner found (A sees B across the cell seam)",
              has(a_sees, B));

        // Move B across a cell boundary and confirm re-bucketing keeps it found.
        g.upsert(B, at(64.0f, 10.0f));  // now cell (1,0)->(1,0) boundary, still ~4 m
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
        AoiGrid g;
        g.upsert(A, at(64.0f, 64.0f));
        g.upsert(B, at(64.0f, 64.0f));  // same spot as A
        g.upsert(C, at(64.0f, 64.0f));
        auto a_sees = g.interest_set(A, kEmpty);
        check("F: A never sees itself", !has(a_sees, A));
        check("F: A sees the two co-located others (B and C)",
              has(a_sees, B) && has(a_sees, C) && a_sees.size() == 2);
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD AOI GRID TESTS PASSED\n"
                            : "\n%d WORLDD AOI GRID TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
