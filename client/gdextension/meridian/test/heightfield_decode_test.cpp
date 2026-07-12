// Project Meridian — the SAME-BYTES proof for the heightfield ground sample
// (issue #557, Epic #22 Story D). This test decodes the Heightfield straight out
// of the REAL shipped `<cx>_<cz>.chunk.bin` (the checked-in Story-0 fixture,
// test/fixtures/chunkpack/) via the flatc-generated chunk.fbs bindings, then
// feeds it to the engine-free HeightfieldWorldQuery. It closes the Q1(a) loop the
// sister test (heightfield_query_test) opens: the client samples the SAME bytes
// worldd validates movement against, so its ground matches the server's by
// construction.
//
// Needs flatc + the FlatBuffers runtime (the login-core discipline in the client
// test CMake); the pure sampler/query test always runs regardless.
//
// Proves:
//   1. Decode round-trips the real buffer (identifier + verifier, 129×129 f32).
//   2. The decoded samples ARE Story-0's non-flat ramp+bowl (match the analytic
//      surface + the manifest AABB Y-span) — a flat-vs-sloped bug fails here.
//   3. Bilinear sampling of the decoded grid matches the analytic surface.
//   4. The shared-edge convention holds across two REAL adjacent chunk files.
//   5. Fail-closed: garbage / truncated bytes decode to false.

#include "heightfield_chunk_decode.h"
#include "heightfield_query.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace mv = meridian::movement;

static int g_fail = 0;
static void check(const char* name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

// Fixture location relative to THIS source (runnable from any build dir), same
// mechanism as chunk_pack_core_test.
static std::string this_dir() {
	std::string f = __FILE__;
	const std::size_t slash = f.find_last_of("/\\");
	return (slash == std::string::npos) ? std::string(".") : f.substr(0, slash);
}
static std::string chunk_bin_path(const char* name) {
	return this_dir() + "/fixtures/chunkpack/meridian/core/chunks/zone01/" + name;
}
static bool read_file(const std::string& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return true;
}

// Story-0 fixture surface (tools/mcc/src/stages/chunk_emit.cpp): origin -384,
// grid_min -512, center -320, 128 m chunks.
static float fixture_height(double wx, double wz) {
	const float ramp = 0.08f * static_cast<float>(wx - (-512.0));
	const float ndx = static_cast<float>((wx - (-320.0)) / 128);
	const float ndz = static_cast<float>((wz - (-320.0)) / 128);
	return 8.0f + ramp + 2.5f * (ndx * ndx + ndz * ndz);
}

int main() {
	std::printf("meridian heightfield decode / same-bytes test (#557)\n");

	// ── 1. Decode the real centre-chunk buffer ──────────────────────────────
	std::string bytes00;
	if (!read_file(chunk_bin_path("0_0.chunk.bin"), bytes00)) {
		std::printf("FATAL: fixture 0_0.chunk.bin missing — cannot run.\n");
		return 2;
	}
	mv::HeightfieldChunk hc00;
	const bool ok00 = mv::decode_heightfield_chunk(bytes00.data(), bytes00.size(), hc00);
	check("decode real 0_0.chunk.bin succeeds", ok00);
	check("decoded coord is (0,0)", hc00.cx == 0 && hc00.cz == 0);
	check("decoded grid is 129x129 @ 1 m", hc00.side == 129 &&
	                                            std::fabs(hc00.spacing_m - 1.0f) < 1e-6f);
	check("decoded sample count is 16641", hc00.samples.size() == 129u * 129u);

	// ── 2. The decoded bytes ARE the non-flat ramp+bowl ─────────────────────
	{
		const int side = 129;
		float max_err = 0.0f, min_h = hc00.samples[0], max_h = hc00.samples[0];
		for (int lz = 0; lz < side; ++lz) {
			const double wz = -384.0 + 0 * 128 + lz;
			for (int lx = 0; lx < side; ++lx) {
				const double wx = -384.0 + 0 * 128 + lx;
				const float stored = hc00.samples[static_cast<std::size_t>(lz) * side + lx];
				const float err = std::fabs(stored - fixture_height(wx, wz));
				if (err > max_err) max_err = err;
				if (stored < min_h) min_h = stored;
				if (stored > max_h) max_h = stored;
			}
		}
		check("decoded samples match the analytic ramp+bowl (<1e-3 m)", max_err < 1e-3f);
		std::printf("      (max decoded-vs-analytic error = %.3e m)\n", max_err);
		// Manifest AABB for 0_0: min y 18.865, max y 29.73 — the surface is sloped,
		// not flat. Span ~10.9 m over the chunk decisively rules out a flat bug.
		check("decoded Y-span matches the manifest AABB (non-flat)",
		      std::fabs(min_h - 18.865f) < 1e-2f && std::fabs(max_h - 29.73f) < 1e-2f);
		std::printf("      (decoded Y range = [%.4f, %.4f])\n", min_h, max_h);
	}

	// ── 3. Bilinear sampling of the DECODED grid matches the analytic surface ─
	{
		mv::HeightfieldWorldQuery world(-384.0f, -384.0f, 128.0f);
		world.add_chunk(hc00);
		struct P { double wx, wz; };
		const P pts[] = {{-384, -384}, {-320.5, -319.5}, {-300.25, -290.75}, {-258, -258}};
		bool ok = true;
		float max_err = 0.0f;
		for (const P& p : pts) {
			const mv::GroundSample g = world.sample_ground(static_cast<float>(p.wx),
			                                               static_cast<float>(p.wz));
			const float err = std::fabs(g.height - fixture_height(p.wx, p.wz));
			if (err > max_err) max_err = err;
			if (!g.walkable || err > 2e-3f) ok = false;
		}
		check("bilinear over the decoded grid matches analytic (<2e-3 m)", ok);
		std::printf("      (max sample-vs-analytic error = %.3e m)\n", max_err);
	}

	// ── 4. Shared-edge across two REAL adjacent chunk files ─────────────────
	{
		std::string bytes10;
		mv::HeightfieldChunk hc10;
		const bool got = read_file(chunk_bin_path("1_0.chunk.bin"), bytes10) &&
		                 mv::decode_heightfield_chunk(bytes10.data(), bytes10.size(), hc10);
		check("decode real 1_0.chunk.bin succeeds", got);
		if (got) {
			const int side = 129;
			bool edge_match = true;
			for (int lz = 0; lz < side; ++lz) {
				const float east = hc00.samples[static_cast<std::size_t>(lz) * side + (side - 1)];
				const float west = hc10.samples[static_cast<std::size_t>(lz) * side + 0];
				if (east != west) edge_match = false;
			}
			check("real 0_0 east edge == real 1_0 west edge (shared-edge)", edge_match);
		}
	}

	// ── 5. Fail-closed on malformed bytes ───────────────────────────────────
	{
		mv::HeightfieldChunk junk;
		const std::string garbage(256, '\x00');
		check("garbage bytes decode to false",
		      !mv::decode_heightfield_chunk(garbage.data(), garbage.size(), junk));
		check("empty buffer decodes to false",
		      !mv::decode_heightfield_chunk(nullptr, 0, junk));
		// A truncated real buffer must not slip past the verifier.
		const std::string truncated = bytes00.substr(0, bytes00.size() / 2);
		check("truncated real buffer decodes to false",
		      !mv::decode_heightfield_chunk(truncated.data(), truncated.size(), junk));
	}

	std::printf(g_fail == 0 ? "\nALL HEIGHTFIELD DECODE TESTS PASSED\n"
	                        : "\n%d HEIGHTFIELD DECODE TEST(S) FAILED\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
