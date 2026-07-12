// Project Meridian — engine-free CHUNK STREAMER core (issue #555, Epic #22
// Story B). The runtime half of the chunk pipeline that sits ON TOP of Story A's
// fail-closed chunk-pack verify (chunk_pack_core.*): once the pack is trusted and
// its IF-6 chunk index is resolved (coords incl. negative, per-chunk priority,
// resolved res:// scene paths), THIS core decides — every tick, from the player's
// world position — which chunks must be resident, drives their asynchronous load
// (priority-ordered), instances the ready ones under a per-frame time-slice
// budget, and unloads (pooled/recycled) the chunks that fell outside the radius.
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores"): this header
// and its .cpp contain NO Godot types — exactly like the #102 movement, #105
// camera, and #554 chunk-pack cores. All ENGINE work (ResourceLoader threaded
// load, PackedScene instancing, Node pooling) is abstracted behind IStreamBackend;
// the GDExtension node (MeridianChunkStream, meridian_chunk_stream.*) implements
// it over Godot, and the unit test (chunk_stream_core_test.cpp) implements it with
// a deterministic fake. So the WHOLE streaming policy — the world→cell math, the
// desired-set enter/leave transitions, the priority load ordering, the per-frame
// instancing budget, and the pooled unload — is proven in a plain C++17 ctest with
// no Godot runtime.
//
// WHY (Client SAD §2.3 streaming; PRD scalability tiers): a zone is far larger
// than one drawable scene, so the client keeps only a ring of chunks around the
// player resident. The ring RADIUS is a per-quality-tier CONFIG value (Q3: Low =
// 1 ring / 3×3, Medium = 2 / 5×5, Epic = 3 / 7×7) — NOT a hardcoded constant — so
// a low-end box streams a tight ring and an Epic box streams a wide one. A burst
// of finished loads must never blow the frame, so instancing is time-sliced to a
// per-frame budget; and chunks leaving the ring are RECYCLED into a pool rather
// than churn-freed, so re-entering a just-left chunk is cheap.

#ifndef MERIDIAN_CHUNK_STREAM_CORE_H
#define MERIDIAN_CHUNK_STREAM_CORE_H

#include <cstddef>
#include <string>
#include <vector>

namespace meridian::stream {

// The quality tiers the streamer knows about. Ordinals MATCH the settings core's
// QualityTier (settings_core.h: Low=0, Medium=1, High=2, Epic=3) so the boot flow
// can pass its selected tier straight through. Q3 pins Low/Medium/Epic radii; High
// defaults between Medium and Epic and is (like all of them) config-overridable.
enum class Tier : int {
	Low    = 0,
	Medium = 1,
	High   = 2,
	Epic   = 3,
};
inline constexpr int kTierCount = 4;

// Per-tier full-detail ring RADII (Chebyshev rings around the player cell). These
// are the CONFIG values Q3 calls for (Low=1 → 3×3, Medium=2 → 5×5, Epic=3 → 7×7),
// exposed so the settings/quality layer can override them per tier — NOT baked
// constants. `radius[tier]` is the ring count for that tier.
struct TierRadiiConfig {
	int radius[kTierCount] = {1, 2, 3, 3};   // Low, Medium, High, Epic (Q3 defaults)

	int radius_for(Tier t) const {
		const int i = static_cast<int>(t);
		return (i >= 0 && i < kTierCount) ? radius[i] : radius[0];
	}
	void set_radius(Tier t, int rings) {
		const int i = static_cast<int>(t);
		if (i >= 0 && i < kTierCount) radius[i] = rings < 0 ? 0 : rings;
	}
};

// One chunk the streamer can manage: its grid coord (may be NEGATIVE — Tools SAD
// §3.1), its load PRIORITY (lower = more urgent; the IF-6 manifest stamps the
// centre/critical chunks priority 0), and the resolved res:// path of its
// baked-mesh scene (Q2 — a baked `.scn`, no runtime Terrain3D). Built by the
// wrapper from Story A's resolved ChunkIndex (ResolvedChunk.cx/cz/priority +
// scene.res_path) — the streamer core does NOT re-parse the manifest.
struct StreamChunk {
	int cx = 0;
	int cz = 0;
	int priority = 0;
	std::string scene_path;
};

// The zone the streamer operates over: the world-space ORIGIN (the minimum corner
// of cell (0,0), in metres — the manifest `origin`), the square chunk edge length
// (`chunk_size_m`), the inclusive grid bounds (for clamping), and every chunk.
struct StreamZone {
	double origin_x = 0.0;
	double origin_z = 0.0;
	int chunk_size_m = 0;
	int min_cx = 0, min_cz = 0, max_cx = 0, max_cz = 0;
	std::vector<StreamChunk> chunks;
};

// The lifecycle state of one managed chunk.
enum class ChunkState {
	Unloaded,   // not requested (or unloaded back to the pool)
	Loading,    // async load requested, awaiting completion
	Ready,      // load finished, resource in hand, awaiting a time-sliced instancing slot
	Instanced,  // instanced into the scene (visible/collidable)
	Failed,     // async load reported failure (surfaced; treated as not-resident)
};

// Result of polling an in-flight async load (mirrors Godot's
// ResourceLoader::ThreadLoadStatus success/progress/fail arms).
enum class LoadPoll {
	InProgress = 0,
	Ready      = 1,
	Failed     = 2,
};

// The ENGINE SEAM. The core issues these calls; the wrapper maps them onto Godot
// (ResourceLoader threaded load, PackedScene instancing, a Node pool), the test
// maps them onto a deterministic fake. `chunk_id` is the chunk's index into
// StreamZone.chunks — a stable handle the backend uses to track its own resources.
struct IStreamBackend {
	virtual ~IStreamBackend() = default;

	// Begin an ASYNC load of scene_path (Godot: ResourceLoader::load_threaded_request).
	// The core issues these in PRIORITY order (most urgent first).
	virtual void request_load(int chunk_id, const std::string &scene_path) = 0;

	// Poll a previously-requested load (Godot: load_threaded_get_status).
	virtual LoadPoll poll_load(int chunk_id, const std::string &scene_path) = 0;

	// INSTANCE the finished scene into the world (Godot: load_threaded_get() →
	// instantiate() → deferred add_child). Called at most `instancing_budget`
	// times per tick — the time-slice. May reuse a pooled node.
	virtual void instantiate(int chunk_id, const std::string &scene_path) = 0;

	// UNLOAD an instanced chunk that left the ring — RECYCLE into the pool, do not
	// churn-free (Godot: remove_child + return the node to a free list).
	virtual void recycle(int chunk_id) = 0;

	// Release a load that was requested/ready but is no longer wanted before it was
	// ever instanced (Godot: drop the threaded-load handle / cached resource).
	virtual void release_load(int chunk_id, const std::string &scene_path) = 0;
};

// The streamer. Configure it with a zone + tier config, then each frame:
//   set_player_position(x, z);  tick(instancing_budget);
// and query the resident/loading/instanced/desired sets. All policy lives here;
// the backend only executes the engine primitives.
class ChunkStreamer {
public:
	ChunkStreamer() = default;

	// Bind the backend (must outlive the streamer). Call once before tick().
	void set_backend(IStreamBackend *backend) { backend_ = backend; }

	// Install the zone to stream. Resets all per-chunk state to Unloaded.
	void configure(const StreamZone &zone);

	// Per-tier ring radii (Q3 config). Replace the whole table or one tier.
	void set_tier_radii(const TierRadiiConfig &cfg) { radii_ = cfg; }
	const TierRadiiConfig &tier_radii() const { return radii_; }
	void set_tier_radius(Tier t, int rings) { radii_.set_radius(t, rings); }

	// The ACTIVE quality tier — selects the ring radius via the config above.
	void set_tier(Tier t) { tier_ = t; }
	Tier tier() const { return tier_; }
	// The active full-detail ring radius (radii_[tier_]).
	int active_radius() const { return radii_.radius_for(tier_); }

	// Update the player's world position (metres, XZ plane). Recomputes the player
	// cell; the desired set is derived on the next tick().
	void set_player_position(double world_x, double world_z);

	// world→cell: the grid cell containing a world XZ point (floor division off the
	// origin; independent of the grid bounds so out-of-range cells are well-defined).
	void world_to_cell(double world_x, double world_z, int &out_cx, int &out_cz) const;

	// Advance the streamer one frame. Issues priority-ordered loads for newly-desired
	// chunks, polls in-flight loads, INSTANCES up to `instancing_budget` ready chunks
	// (the per-frame time-slice), and recycles chunks that left the ring. Returns the
	// number of chunks instanced THIS tick (always ≤ instancing_budget).
	int tick(int instancing_budget);

	// ── Queries (drive the headless verify) ──────────────────────────────────
	int  player_cx() const { return pcx_; }
	int  player_cz() const { return pcz_; }
	bool has_player() const { return has_player_; }

	std::size_t chunk_count() const { return chunks_.size(); }
	ChunkState  state_at(int cx, int cz) const;             // Unloaded if not in zone
	int         chunk_id_at(int cx, int cz) const;          // -1 if not in zone

	// A read-only view of one managed chunk by id (0..chunk_count()-1) — its coord,
	// priority, and current state — so a caller (the wrapper's cell queries, a HUD)
	// can enumerate chunks without re-deriving the grid.
	struct ChunkView {
		int cx = 0, cz = 0, priority = 0;
		ChunkState state = ChunkState::Unloaded;
	};
	ChunkView view(std::size_t id) const;

	// Is (cx,cz) inside the current desired ring (within active_radius of the player
	// cell AND present in the zone)?
	bool is_desired(int cx, int cz) const;

	// Counts by state (across all managed chunks).
	std::size_t count_state(ChunkState s) const;
	std::size_t desired_count() const;
	std::size_t loading_count() const   { return count_state(ChunkState::Loading); }
	std::size_t ready_count() const     { return count_state(ChunkState::Ready); }
	std::size_t instanced_count() const { return count_state(ChunkState::Instanced); }
	// Resident = any chunk the streamer is actively holding (loading, ready, or
	// instanced) — i.e. consuming a slot toward the ring.
	std::size_t resident_count() const;

	// The most recent tick's instancing count (for the budget assertion).
	int last_instanced_this_tick() const { return last_instanced_; }
	// Cumulative counters (diagnostics / verify).
	long long total_loads_requested() const { return total_loads_; }
	long long total_instanced() const       { return total_instanced_; }
	long long total_recycled() const        { return total_recycled_; }
	long long total_load_failures() const   { return total_failures_; }

private:
	struct Slot {
		StreamChunk chunk;
		ChunkState  state = ChunkState::Unloaded;
	};

	// Chebyshev ring test around the current player cell.
	bool within_ring(int cx, int cz) const;

	IStreamBackend *backend_ = nullptr;
	StreamZone      zone_;
	std::vector<Slot> chunks_;
	TierRadiiConfig radii_;
	Tier            tier_ = Tier::Medium;

	bool has_player_ = false;
	int  pcx_ = 0, pcz_ = 0;

	int  last_instanced_ = 0;
	long long total_loads_ = 0;
	long long total_instanced_ = 0;
	long long total_recycled_ = 0;
	long long total_failures_ = 0;
};

// Human-readable state name (logs / test diagnostics).
const char *chunk_state_name(ChunkState s);

}  // namespace meridian::stream

#endif  // MERIDIAN_CHUNK_STREAM_CORE_H
