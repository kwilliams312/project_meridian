// Project Meridian — engine-free unit test for the CHUNK STREAMER core (issue
// #555, Epic #22 Story B). NO Godot: compiles against the plain-C++ core
// (chunk_stream_core.*), so it runs in any C++17 toolchain (Client SAD §9.2).
// Plain-main style, mirroring the chunk-pack / movement tests.
//
// The zone under test is the SHAPE of Story-0's `mcc chunk-emit` fixture (the same
// grid the wrapper feeds the core from Story A's resolved index): a 3×3 grid,
// origin {-384,-384}, chunk_size_m 128, coords incl. NEGATIVE, centre chunk (0,0)
// priority 0 and the ring priority 1.
//
// Proves the #555 streamer contract:
//   (a) world→cell floor math (incl. negative cells + exact cell edges);
//   (b) the desired ring per TIER radius (Q3: Low 3×3 / Medium 5×5 / Epic 7×7),
//       configurable — not constant;
//   (c) desired-set ENTER/LEAVE transitions as a synthetic player crosses cell
//       boundaries across the grid;
//   (d) loads issued in PRIORITY order (centre chunk first);
//   (e) the per-frame time-sliced INSTANCING BUDGET is never exceeded, and every
//       desired chunk eventually instances;
//   (f) chunks leaving the ring are RECYCLED into the pool and REUSED on re-entry
//       (pooled unload — not churn-freed); and
//   (g) a load that leaves the ring before instancing is RELEASED (not recycled).
//
// Exit code 0 = all pass; non-zero = at least one failure.

#include "chunk_stream_core.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace st = meridian::stream;

static int g_fail = 0;
static int g_checks = 0;

static void check(const char *name, bool ok, const std::string &detail = "") {
	++g_checks;
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) {
		++g_fail;
		if (!detail.empty()) std::printf("        %s\n", detail.c_str());
	}
}

// ── Deterministic fake backend: models async load (ready after N polls), instancing,
//    and a per-chunk recycle pool — the engine primitives, without Godot. ────────
struct FakeBackend final : st::IStreamBackend {
	int ready_delay = 0;   // polls before a requested load reports Ready (0 = next poll)

	std::unordered_map<int, int>  polls;          // chunk_id -> polls seen since request
	std::unordered_set<int>       requested;      // loads in flight / ready
	std::unordered_set<int>       pool;           // recycled instances awaiting reuse
	std::unordered_set<int>       live;           // instanced (attached) chunks

	// Bookkeeping the test asserts on.
	std::vector<int> request_order;               // request_load order (this tick, cleared by test)
	int instantiate_calls = 0;                    // instantiate calls (this tick, cleared by test)
	int reuse_count = 0;                          // times instantiate reused a pooled instance
	int recycle_count = 0;                        // recycle calls
	int release_count = 0;                        // release_load calls
	long long total_requests = 0;

	void reset_tick_counters() {
		request_order.clear();
		instantiate_calls = 0;
	}

	void request_load(int chunk_id, const std::string &) override {
		requested.insert(chunk_id);
		polls[chunk_id] = 0;
		request_order.push_back(chunk_id);
		++total_requests;
	}
	st::LoadPoll poll_load(int chunk_id, const std::string &) override {
		int &p = polls[chunk_id];
		++p;
		return (p > ready_delay) ? st::LoadPoll::Ready : st::LoadPoll::InProgress;
	}
	void instantiate(int chunk_id, const std::string &) override {
		++instantiate_calls;
		auto it = pool.find(chunk_id);
		if (it != pool.end()) {
			pool.erase(it);
			++reuse_count;       // reused a recycled instance — no fresh alloc
		}
		live.insert(chunk_id);
		requested.erase(chunk_id);
	}
	void recycle(int chunk_id) override {
		live.erase(chunk_id);
		pool.insert(chunk_id);   // detached, kept — NOT freed
		++recycle_count;
	}
	void release_load(int chunk_id, const std::string &) override {
		requested.erase(chunk_id);
		++release_count;
	}
};

// ── The 3×3 fixture-shaped zone (origin -384, size 128, centre priority 0) ─────
static st::StreamZone make_zone() {
	st::StreamZone z;
	z.origin_x = -384.0;
	z.origin_z = -384.0;
	z.chunk_size_m = 128;
	z.min_cx = -1; z.min_cz = -1; z.max_cx = 1; z.max_cz = 1;
	for (int cz = -1; cz <= 1; ++cz) {
		for (int cx = -1; cx <= 1; ++cx) {
			st::StreamChunk c;
			c.cx = cx; c.cz = cz;
			c.priority = (cx == 0 && cz == 0) ? 0 : 1;
			c.scene_path = "res://meridian/core/chunks/zone01/" +
					std::string(cx < 0 ? "n" : "") + std::to_string(cx < 0 ? -cx : cx) + "_" +
					std::string(cz < 0 ? "n" : "") + std::to_string(cz < 0 ? -cz : cz) + ".scn";
			z.chunks.push_back(std::move(c));
		}
	}
	return z;
}

// Run one tick, resetting the fake's per-tick counters first (mirrors a frame).
static int frame(st::ChunkStreamer &s, FakeBackend &b, int budget) {
	b.reset_tick_counters();
	return s.tick(budget);
}

int main() {
	std::printf("meridian chunk STREAMER core test (#555)\n");

	// ── (a) world→cell floor math ─────────────────────────────────────────────
	{
		st::ChunkStreamer s;
		s.configure(make_zone());
		int cx = 0, cz = 0;
		s.world_to_cell(-320.0, -320.0, cx, cz);
		check("cell of (-320,-320) is (0,0)", cx == 0 && cz == 0);
		s.world_to_cell(-450.0, -450.0, cx, cz);
		check("cell of (-450,-450) is (-1,-1) [negative]", cx == -1 && cz == -1);
		s.world_to_cell(-200.0, -200.0, cx, cz);
		check("cell of (-200,-200) is (1,1)", cx == 1 && cz == 1);
		s.world_to_cell(-384.0, -384.0, cx, cz);
		check("cell of exact origin (-384,-384) is (0,0)", cx == 0 && cz == 0);
		s.world_to_cell(-384.5, -384.5, cx, cz);
		check("cell just below origin is (-1,-1)", cx == -1 && cz == -1);
	}

	// ── (b) desired ring per tier radius (Q3), configurable ───────────────────
	{
		st::ChunkStreamer s;
		s.configure(make_zone());
		s.set_player_position(-320.0, -320.0);  // cell (0,0)

		s.set_tier(st::Tier::Low);
		check("Low radius == 1 (3x3)", s.active_radius() == 1);
		check("Low desired = all 9 at centre", s.desired_count() == 9);

		s.set_tier(st::Tier::Medium);
		check("Medium radius == 2 (5x5)", s.active_radius() == 2);
		check("Medium desired = all 9 (grid smaller than ring)", s.desired_count() == 9);

		s.set_tier(st::Tier::Epic);
		check("Epic radius == 3 (7x7)", s.active_radius() == 3);

		// Radii are CONFIG, not constants — override and observe.
		s.set_tier_radius(st::Tier::Low, 0);
		s.set_tier(st::Tier::Low);
		check("Low radius overridable to 0 (1x1)", s.active_radius() == 0);
		check("radius-0 desired = 1 (only the player cell)", s.desired_count() == 1);
	}

	// ── (b2) desired set shrinks at a grid corner ─────────────────────────────
	{
		st::ChunkStreamer s;
		s.configure(make_zone());
		s.set_tier(st::Tier::Low);
		s.set_player_position(-450.0, -450.0);   // cell (-1,-1), grid corner
		check("Low at corner (-1,-1) desired = 4", s.desired_count() == 4);
		check("  (-1,-1) desired", s.is_desired(-1, -1));
		check("  (0,0) desired", s.is_desired(0, 0));
		check("  (1,1) NOT desired (cheb 2)", !s.is_desired(1, 1));
		check("  (1,0) NOT desired", !s.is_desired(1, 0));
	}

	// ── (d) loads issued in PRIORITY order (centre priority 0 first) ───────────
	{
		st::ChunkStreamer s;
		FakeBackend b;
		s.set_backend(&b);
		s.configure(make_zone());
		s.set_tier(st::Tier::Low);
		s.set_player_position(-320.0, -320.0);   // centre
		frame(s, b, /*budget*/2);                 // tick 1: requests all 9
		check("first tick requested all 9 chunks", b.request_order.size() == 9,
				"got " + std::to_string(b.request_order.size()));
		// The first request must be the centre chunk (priority 0).
		bool centre_first = false;
		if (!b.request_order.empty()) {
			const st::ChunkStreamer::ChunkView v = s.view(static_cast<std::size_t>(b.request_order.front()));
			centre_first = (v.cx == 0 && v.cz == 0);
		}
		check("centre chunk (priority 0) requested FIRST", centre_first);
	}

	// ── (e) per-frame instancing budget never exceeded; all eventually instance ─
	{
		st::ChunkStreamer s;
		FakeBackend b;
		b.ready_delay = 0;                        // ready on first poll
		s.set_backend(&b);
		s.configure(make_zone());
		s.set_tier(st::Tier::Low);
		s.set_player_position(-320.0, -320.0);   // 9 desired
		const int budget = 2;

		bool budget_ok = true;
		int guard = 0;
		// Drive frames until all 9 are instanced (or a guard trips).
		while (s.instanced_count() < 9 && guard < 50) {
			const int inst = frame(s, b, budget);
			if (inst > budget) budget_ok = false;
			if (b.instantiate_calls > budget) budget_ok = false;
			if (s.last_instanced_this_tick() > budget) budget_ok = false;
			++guard;
		}
		check("instancing budget (2) never exceeded in any frame", budget_ok);
		check("all 9 chunks eventually instanced", s.instanced_count() == 9,
				"instanced=" + std::to_string(s.instanced_count()));
		check("no chunk left loading/ready once settled", s.resident_count() == 9);
		// 9 chunks at budget 2 → first instancing frame is tick 2, then 2/2/2/2/1.
		check("took the expected number of frames (>=6)", guard >= 6);
	}

	// ── (c) enter/leave transitions across a player path ──────────────────────
	{
		st::ChunkStreamer s;
		FakeBackend b;
		b.ready_delay = 0;
		s.set_backend(&b);
		s.configure(make_zone());
		s.set_tier(st::Tier::Low);

		// Settle fully instanced at the centre.
		s.set_player_position(-320.0, -320.0);
		for (int i = 0; i < 12; ++i) frame(s, b, 4);
		check("settled: 9 instanced at centre", s.instanced_count() == 9);
		check("  chunk (-1,-1) instanced at centre", s.state_at(-1, -1) == st::ChunkState::Instanced);

		// Walk to the +X,+Z corner cell (1,1): (-1,-1) leaves the ring.
		s.set_player_position(-200.0, -200.0);   // cell (1,1)
		check("moved: player cell is (1,1)", s.player_cx() == 1 && s.player_cz() == 1);
		check("  (-1,-1) no longer desired", !s.is_desired(-1, -1));
		check("  (2,2)-ish still bounded; (1,1) desired", s.is_desired(1, 1));
		const long long recy_before = s.total_recycled();
		frame(s, b, 4);                           // leave transition fires
		check("  (-1,-1) recycled to Unloaded", s.state_at(-1, -1) == st::ChunkState::Unloaded);
		check("  a recycle happened", s.total_recycled() > recy_before);
		// At (1,1) Low, desired = cells cheb<=1 ∩ grid = {0,1}×{0,1} = 4.
		for (int i = 0; i < 8; ++i) frame(s, b, 4);
		check("  desired at (1,1) corner = 4", s.desired_count() == 4);
		check("  instanced settles to 4", s.instanced_count() == 4,
				"instanced=" + std::to_string(s.instanced_count()));
	}

	// ── (f) pooled unload → REUSE on re-entry (recycle, not churn-free) ───────
	{
		st::ChunkStreamer s;
		FakeBackend b;
		b.ready_delay = 0;
		s.set_backend(&b);
		s.configure(make_zone());
		s.set_tier(st::Tier::Low);

		// Instance all 9 at centre.
		s.set_player_position(-320.0, -320.0);
		for (int i = 0; i < 12; ++i) frame(s, b, 4);
		check("f: 9 instanced", s.instanced_count() == 9);
		const int reuse_before = b.reuse_count;

		// Teleport far outside the grid → all 9 leave and recycle into the pool.
		s.set_player_position(100000.0, 100000.0);
		frame(s, b, 4);
		check("f: all recycled to the pool", static_cast<int>(b.pool.size()) == 9,
				"pool=" + std::to_string(b.pool.size()));
		check("f: nothing instanced away from grid", s.instanced_count() == 0);
		check("f: no live instances in backend", b.live.empty());

		// Return to centre → chunks reload then INSTANCE by REUSING pooled nodes.
		s.set_player_position(-320.0, -320.0);
		for (int i = 0; i < 12; ++i) frame(s, b, 4);
		check("f: 9 instanced again on return", s.instanced_count() == 9);
		check("f: instances came from the POOL (reuse == 9)", b.reuse_count - reuse_before == 9,
				"reuse delta=" + std::to_string(b.reuse_count - reuse_before));
		check("f: pool drained by reuse", b.pool.empty());
	}

	// ── (g) a load leaving before instancing is RELEASED, not recycled ────────
	{
		st::ChunkStreamer s;
		FakeBackend b;
		b.ready_delay = 0;
		s.set_backend(&b);
		s.configure(make_zone());
		s.set_tier(st::Tier::Low);
		s.set_player_position(-320.0, -320.0);

		// budget 0 → loads reach Ready but NEVER instance.
		frame(s, b, 0);   // enter: request 9 (Loading)
		frame(s, b, 0);   // poll: 9 -> Ready; still no instancing
		check("g: 9 ready, 0 instanced (budget 0)", s.ready_count() == 9 && s.instanced_count() == 0);
		const int rel_before = b.release_count;
		const int recy_before = b.recycle_count;

		// Teleport away → the ready-but-uninstanced loads are RELEASED.
		s.set_player_position(100000.0, 100000.0);
		frame(s, b, 0);
		check("g: released the un-instanced loads", b.release_count - rel_before == 9,
				"release delta=" + std::to_string(b.release_count - rel_before));
		check("g: nothing recycled (never instanced)", b.recycle_count - recy_before == 0);
		check("g: all back to Unloaded", s.resident_count() == 0);
	}

	std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
