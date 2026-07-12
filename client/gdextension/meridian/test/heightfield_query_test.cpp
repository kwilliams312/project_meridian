// Project Meridian — engine-free unit test for the M1 heightfield ground-sample
// backend (issue #557, Epic #22 Story D). NO Godot, NO FlatBuffers: it compiles
// against the plain-C++ core (heightfield_query.*) + the #101 headers only, so it
// runs in any C++17 toolchain (Client SAD §9.2 engine-agnostic cores). Plain-main
// style, mirroring the server tests + movement_controller_test.
//
// Proves the #557 deliverables against Story-0's KNOWN non-flat fixture surface —
// a gentle eastward ramp + a shallow parabolic bowl, a PURE FUNCTION of zone-local
// world coords (tools/mcc/src/stages/chunk_emit.cpp fixture_height, verified here
// against the checked-in fixture AABBs). This test rebuilds that exact surface in
// memory; the SISTER test (heightfield_decode_test) proves the SAME bytes come out
// of the real shipped `.chunk.bin`.
//
//   1. Bilinear-sample PARITY vs the analytic ramp/bowl (lattice-exact + in-cell).
//   2. The DOCUMENTED bilinear formula parity (the sampler == the spec, byte-for-
//      byte — the routine the server track shares/mirrors for client↔server parity).
//   3. NaN hole -> not walkable; out-of-bounds -> not walkable.
//   4. Shared-edge convention (chunk seam samples identically from either chunk).

#include "heightfield_query.h"
#include "movement_constants.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace mv = meridian::movement;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}
static bool near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps;
}

// ── Story-0 fixture surface (tools/mcc/src/stages/chunk_emit.cpp) ─────────────
// Zone-01 defaults: origin -384, 3×3 grid, 128 m chunks ⇒ grid_min = -512,
// center = -320. Height = 8 + 0.08*(wx - grid_min_x) + 2.5*(ndx² + ndz²), a pure
// function of world coords so shared edges join with no seam. Reproduced in the
// SAME float order as the exporter (verified: the corner values below match the
// fixture manifest AABBs, e.g. chunk 0_0 min y = 18.865, max y = 29.73).
static constexpr double kOrigin   = -384.0;
static constexpr double kGridMinX = -512.0;
static constexpr double kCenter   = -320.0;
static constexpr int    kChunk    = 128;
static constexpr int    kSide     = 129;

static float fixture_height(double wx, double wz) {
	const float ramp = 0.08f * static_cast<float>(wx - kGridMinX);
	const float ndx = static_cast<float>((wx - kCenter) / kChunk);
	const float ndz = static_cast<float>((wz - kCenter) / kChunk);
	const float bowl = 2.5f * (ndx * ndx + ndz * ndz);
	return 8.0f + ramp + bowl;
}

static mv::HeightfieldChunk build_chunk(int cx, int cz) {
	mv::HeightfieldChunk c;
	c.cx = cx;
	c.cz = cz;
	c.side = static_cast<std::uint16_t>(kSide);
	c.spacing_m = 1.0f;
	c.samples.resize(static_cast<std::size_t>(kSide) * kSide);
	for (int lz = 0; lz < kSide; ++lz) {
		const double wz = kOrigin + static_cast<double>(cz) * kChunk + lz;
		for (int lx = 0; lx < kSide; ++lx) {
			const double wx = kOrigin + static_cast<double>(cx) * kChunk + lx;
			c.samples[static_cast<std::size_t>(lz) * kSide + lx] = fixture_height(wx, wz);
		}
	}
	return c;
}

int main() {
	std::printf("meridian heightfield world-query core test (#557)\n");

	// The resident set: the centre chunk + its east and north neighbours, so the
	// seams and the world->cell mapping across chunk boundaries are exercised.
	mv::HeightfieldChunk c00 = build_chunk(0, 0);
	mv::HeightfieldChunk c10 = build_chunk(1, 0);
	mv::HeightfieldChunk c01 = build_chunk(0, 1);
	mv::HeightfieldWorldQuery world(static_cast<float>(kOrigin),
	                                static_cast<float>(kOrigin),
	                                static_cast<float>(kChunk));
	world.add_chunk(c00);
	world.add_chunk(c10);
	world.add_chunk(c01);
	check("three chunks resident", world.chunk_count() == 3);

	// =======================================================================
	// 1. LATTICE-EXACT parity: at an integer world coord the sample maps to a
	//    grid lattice point and returns EXACTLY the stored (analytic) height.
	// =======================================================================
	std::printf("[1] bilinear parity vs analytic ramp/bowl\n");
	{
		struct P { double wx, wz; };
		const P pts[] = {{-384, -384}, {-380, -340}, {-320, -320}, {-300, -288}, {-258, -300}};
		bool all_exact = true;
		for (const P& p : pts) {
			const mv::GroundSample g = world.sample_ground(static_cast<float>(p.wx),
			                                               static_cast<float>(p.wz));
			const float analytic = fixture_height(p.wx, p.wz);
			if (!g.walkable || g.height != analytic) all_exact = false;
		}
		check("lattice-point samples equal the analytic height exactly", all_exact);

		// In-cell (non-lattice) points: bilinear tracks the analytic surface to a
		// tight tolerance (the ramp is exactly linear; the bowl's per-cell
		// quadratic curvature error is < 1e-4 m).
		struct Q { double wx, wz; };
		const Q mid[] = {{-320.5, -319.5}, {-300.25, -290.75}, {-383.5, -383.5}};
		bool all_close = true;
		float max_err = 0.0f;
		for (const Q& q : mid) {
			const mv::GroundSample g = world.sample_ground(static_cast<float>(q.wx),
			                                               static_cast<float>(q.wz));
			const float analytic = fixture_height(q.wx, q.wz);
			const float err = std::fabs(g.height - analytic);
			if (err > max_err) max_err = err;
			if (!g.walkable || err > 2e-3f) all_close = false;
		}
		check("in-cell samples track the analytic surface within 2e-3 m", all_close);
		std::printf("      (max in-cell bilinear-vs-analytic error = %.3e m)\n", max_err);

		// Non-flat proof: the sampled ground actually varies across the chunk (a
		// flat-vs-sloped regression — the whole point of a non-flat fixture — is
		// caught here). Height rises monotonically eastward (ramp dominates).
		const float h0 = world.sample_ground(-384.0f, -320.0f).height;
		const float h1 = world.sample_ground(-350.0f, -320.0f).height;
		const float h2 = world.sample_ground(-300.0f, -320.0f).height;
		const float h3 = world.sample_ground(-258.0f, -320.0f).height;
		check("ground is non-flat (varies eastward)", (h3 - h0) > 1.0f);
		check("ground rises monotonically eastward (ramp)",
		      h0 < h1 && h1 < h2 && h2 < h3);
	}

	// =======================================================================
	// 2. DOCUMENTED-FORMULA parity: the sampler reproduces the exact bilinear
	//    formula in the header (the routine the server track shares/mirrors, so
	//    client prediction and server validation agree by construction).
	// =======================================================================
	std::printf("[2] shared sampler == documented bilinear formula\n");
	{
		// Sample the centre chunk directly at a non-lattice local offset and
		// recompute the four-corner bilinear by hand — must be BIT-IDENTICAL.
		const float local_x = 37.3f, local_z = 88.6f;
		const mv::HeightfieldSampleResult r = mv::sample_heightfield_bilinear(
		    c00.samples.data(), c00.side, c00.spacing_m, local_x, local_z);

		const int side = kSide;
		const float gx = local_x / 1.0f, gz = local_z / 1.0f;
		const int ix = static_cast<int>(std::floor(gx));
		const int iz = static_cast<int>(std::floor(gz));
		const float fx = gx - static_cast<float>(ix);
		const float fz = gz - static_cast<float>(iz);
		const float h00 = c00.samples[static_cast<std::size_t>(iz) * side + ix];
		const float h10 = c00.samples[static_cast<std::size_t>(iz) * side + ix + 1];
		const float h01 = c00.samples[static_cast<std::size_t>(iz + 1) * side + ix];
		const float h11 = c00.samples[static_cast<std::size_t>(iz + 1) * side + ix + 1];
		const float ref = (h00 + (h10 - h00) * fx) +
		                  ((h01 + (h11 - h01) * fx) - (h00 + (h10 - h00) * fx)) * fz;
		check("sampler output is bit-identical to the documented formula",
		      !r.hole && r.height == ref);

		// A lattice offset returns the stored sample exactly (fx == fz == 0).
		const mv::HeightfieldSampleResult latt = mv::sample_heightfield_bilinear(
		    c00.samples.data(), c00.side, c00.spacing_m, 40.0f, 90.0f);
		check("lattice offset returns the stored sample exactly",
		      !latt.hole && latt.height == c00.samples[static_cast<std::size_t>(90) * side + 40]);
	}

	// =======================================================================
	// 3. HOLES and OUT-OF-BOUNDS -> not walkable (never a silent flat guess).
	// =======================================================================
	std::printf("[3] NaN holes + out-of-bounds are not walkable\n");
	{
		// Poke a NaN into the centre chunk and sample INSIDE the affected cell.
		mv::HeightfieldChunk holed = build_chunk(0, 0);
		const int side = kSide;
		const int hz = 50, hx = 60;   // corner of the cell [hx,hx+1] x [hz,hz+1]
		holed.samples[static_cast<std::size_t>(hz) * side + hx] =
		    std::numeric_limits<float>::quiet_NaN();
		mv::HeightfieldWorldQuery hw(static_cast<float>(kOrigin),
		                             static_cast<float>(kOrigin),
		                             static_cast<float>(kChunk));
		hw.add_chunk(holed);
		// World coord landing inside that cell: wx = origin + hx + 0.5, etc.
		const float wx = static_cast<float>(kOrigin) + hx + 0.5f;
		const float wz = static_cast<float>(kOrigin) + hz + 0.5f;
		const mv::GroundSample hole = hw.sample_ground(wx, wz);
		check("sample over a NaN hole is not walkable", !hole.walkable);
		// A cell NOT touching the hole in the same chunk is still walkable.
		const mv::GroundSample solid = hw.sample_ground(
		    static_cast<float>(kOrigin) + 10.5f, static_cast<float>(kOrigin) + 10.5f);
		check("a solid cell in the same chunk is walkable", solid.walkable);

		// Out of bounds: a world XZ in a chunk cell that is NOT resident.
		const mv::GroundSample oob_cell = world.sample_ground(-500.0f, -320.0f);  // cx=-1 (not added)
		check("XZ in a non-resident chunk cell is not walkable", !oob_cell.walkable);
		const mv::GroundSample oob_far = world.sample_ground(5000.0f, 5000.0f);
		check("XZ far outside the grid is not walkable", !oob_far.walkable);
	}

	// =======================================================================
	// 4. SHARED-EDGE convention: the seam samples identically from either chunk.
	// =======================================================================
	std::printf("[4] shared-edge convention holds at chunk seams\n");
	{
		// Grid-level: chunk 0_0 east column (lx=128) == chunk 1_0 west column (lx=0)
		// for every row — the schema §3.2 shared-edge guarantee the sampler relies on.
		const int side = kSide;
		bool edge_match = true;
		for (int lz = 0; lz < side; ++lz) {
			const float east = c00.samples[static_cast<std::size_t>(lz) * side + (side - 1)];
			const float west = c10.samples[static_cast<std::size_t>(lz) * side + 0];
			if (east != west) edge_match = false;
		}
		check("0_0 east edge == 1_0 west edge for every row", edge_match);

		// Query-level: sampling just inside chunk 0_0 vs just inside chunk 1_0 at
		// the shared seam (world x = -256) yields the same height (continuous).
		const float seam_z = -300.0f;
		const mv::GroundSample from00 = world.sample_ground(-256.001f, seam_z);
		const mv::GroundSample from10 = world.sample_ground(-256.0f, seam_z);
		check("ground is continuous across the 0_0 | 1_0 seam",
		      from00.walkable && from10.walkable &&
		          near(from00.height, from10.height, 1e-2f));
	}

	std::printf(g_fail == 0 ? "\nALL HEIGHTFIELD QUERY TESTS PASSED\n"
	                        : "\n%d HEIGHTFIELD QUERY TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
