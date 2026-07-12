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
#include <limits>
#include <string>
#include <vector>

namespace meridian::stream {

// Which representation of a chunk the streamer is loading / showing (issue #556,
// Epic #22 Story C). Beyond the FULL-detail ring the streamer keeps a PROXY
// far-ring band: a baked low-poly `.proxy.scn` (Client SAD §2.3 "beyond the
// radius: baked proxy meshes from the same manifest"). A chunk with an explicit
// `proxy: null` (chunk-pack amendment C3) has no proxy — the far-ring band shows
// NOTHING for it (Rep::None). As a chunk crosses the full-detail boundary the
// streamer SWAPS proxy↔full (SAD §3d: full replaces proxy on stream-in; proxy
// re-shown on stream-out), holding the current representation visible until the
// new one is instanced so there is never a visible gap.
enum class Rep : int {
	None  = 0,   // nothing to load/show (outside the far-ring, or proxy:null in the band)
	Proxy = 1,   // the baked low-poly proxy mesh (far-ring band)
	Full  = 2,   // the full baked-mesh chunk (full-detail ring)
};

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

// Per-tier ring RADII (Chebyshev rings around the player cell), both CONFIG — NOT
// baked constants — so the settings/quality layer overrides them per tier.
//   * radius[tier]   — the FULL-detail ring (Q3: Low=1 → 3×3, Medium=2 → 5×5,
//                      Epic=3 → 7×7). Inside it a chunk shows its full baked mesh.
//   * far_ring[tier] — the PROXY far-ring (Story C / Q3: Low=3, Medium=4, Epic=6;
//                      High mirrors Epic). From radius+1 out to far_ring a chunk
//                      shows its low-poly proxy (Rep::Proxy) instead of the full
//                      mesh; beyond far_ring it is unloaded. far_ring is expected
//                      to be ≥ radius for every tier (a proxy band never narrower
//                      than the full ring); the manifest's zone-level `far_ring`
//                      is the pack's baked cap, this is the per-tier draw budget.
struct TierRadiiConfig {
	int radius[kTierCount]   = {1, 2, 3, 3};   // Low, Medium, High, Epic full-detail (Q3)
	int far_ring[kTierCount] = {3, 4, 6, 6};   // Low, Medium, High, Epic proxy far-ring (Q3)

	int radius_for(Tier t) const {
		const int i = static_cast<int>(t);
		return (i >= 0 && i < kTierCount) ? radius[i] : radius[0];
	}
	void set_radius(Tier t, int rings) {
		const int i = static_cast<int>(t);
		if (i >= 0 && i < kTierCount) radius[i] = rings < 0 ? 0 : rings;
	}
	int far_ring_for(Tier t) const {
		const int i = static_cast<int>(t);
		return (i >= 0 && i < kTierCount) ? far_ring[i] : far_ring[0];
	}
	void set_far_ring(Tier t, int rings) {
		const int i = static_cast<int>(t);
		if (i >= 0 && i < kTierCount) far_ring[i] = rings < 0 ? 0 : rings;
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
	std::string scene_path;              // full baked-mesh scene (res://…scene .scn)
	// Story C: the low-poly PROXY scene for the far-ring band. `has_proxy` is false
	// for the explicit `proxy: null` chunk (chunk-pack amendment C3) — such a chunk
	// shows NOTHING in the proxy band. Built by the wrapper from Story A's
	// ResolvedChunk.has_proxy / proxy.res_path.
	bool has_proxy = false;
	std::string proxy_path;              // valid only when has_proxy
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
//
// Story C: every call also carries the REPRESENTATION (`rep`, Proxy or Full) it
// acts on — a single chunk can hold a proxy AND a full transiently during a
// gapless swap, and the backend pools each representation separately (a proxy
// node recycled for chunk_id must never be reused as that chunk's full mesh). The
// core never issues a seam call with Rep::None.
struct IStreamBackend {
	virtual ~IStreamBackend() = default;

	// Begin an ASYNC load of scene_path (Godot: ResourceLoader::load_threaded_request).
	// The core issues these in PRIORITY order (most urgent first).
	virtual void request_load(int chunk_id, Rep rep, const std::string &scene_path) = 0;

	// Poll a previously-requested load (Godot: load_threaded_get_status).
	virtual LoadPoll poll_load(int chunk_id, Rep rep, const std::string &scene_path) = 0;

	// INSTANCE the finished scene into the world (Godot: load_threaded_get() →
	// instantiate() → deferred add_child). Called at most `instancing_budget`
	// times per tick — the time-slice, shared across proxy AND full. May reuse a
	// pooled node for (chunk_id, rep).
	virtual void instantiate(int chunk_id, Rep rep, const std::string &scene_path) = 0;

	// UNLOAD an instanced (chunk_id, rep) that left its band or was swapped out —
	// RECYCLE into the pool, do not churn-free (Godot: remove_child + return the
	// node to a per-representation free list).
	virtual void recycle(int chunk_id, Rep rep) = 0;

	// Release a load that was requested/ready but is no longer wanted before it was
	// ever instanced (Godot: drop the threaded-load handle / cached resource).
	virtual void release_load(int chunk_id, Rep rep, const std::string &scene_path) = 0;
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

	// Per-tier ring radii (Q3 config). Replace the whole table or one tier. Covers
	// BOTH the full-detail radius and the proxy far-ring (Story C).
	void set_tier_radii(const TierRadiiConfig &cfg) { radii_ = cfg; }
	const TierRadiiConfig &tier_radii() const { return radii_; }
	void set_tier_radius(Tier t, int rings) { radii_.set_radius(t, rings); }
	// The proxy far-ring for a tier (Story C / Q3: Low 3 / Medium 4 / Epic 6).
	void set_tier_far_ring(Tier t, int rings) { radii_.set_far_ring(t, rings); }

	// The ACTIVE quality tier — selects both ring radii via the config above.
	void set_tier(Tier t) { tier_ = t; }
	Tier tier() const { return tier_; }
	// The active full-detail ring radius (radii_.radius[tier_]).
	int active_radius() const { return radii_.radius_for(tier_); }
	// The active proxy far-ring radius (radii_.far_ring[tier_]).
	int active_far_ring() const { return radii_.far_ring_for(tier_); }

	// Update the player's world position (metres, XZ plane). Recomputes the player
	// cell; the desired set is derived on the next tick().
	void set_player_position(double world_x, double world_z);

	// world→cell: the grid cell containing a world XZ point (floor division off the
	// origin; independent of the grid bounds so out-of-range cells are well-defined).
	void world_to_cell(double world_x, double world_z, int &out_cx, int &out_cz) const;

	// Advance the streamer one frame. Issues priority-ordered loads for newly-desired
	// chunks (full inside the radius, proxy in the far-ring band), polls in-flight
	// loads, INSTANCES up to `instancing_budget` ready representations (the per-frame
	// time-slice — proxy AND full share the one budget, Story C), completing any
	// proxy↔full swap gaplessly, and recycles representations that left their band.
	// Returns the number of representations instanced THIS tick (always ≤ budget).
	int tick(int instancing_budget);

	// ── Hitch gate (Story C, Client SAD §3d / §6.3: no frame > 50 ms attributable
	//    to streaming). Instancing is the only streaming work on the main thread, so
	//    the streaming-attributable frame cost is (instancings this tick) × (per-
	//    instancing main-thread cost). The core is engine-free so it cannot time a
	//    real GPU upload — instead it models a per-instancing cost (set from a
	//    measured value; real tier-machine numbers land via the perf fleet #31) and
	//    exposes the resulting frame cost so a HUD or a headless harness can assert
	//    the ≤ 50 ms gate. The cap itself is ENFORCED structurally by never issuing
	//    more than `instancing_budget` instantiations per tick. ─────────────────────
	static constexpr double kHitchGateMs = 50.0;   // Client SAD §3d / D-05 (Low, SATA SSD)

	// Model the per-instancing main-thread cost in milliseconds (default 0 → cost
	// tracking off, so callers that don't measure are unaffected). Set from a
	// measured instancing time to make last_stream_frame_cost_ms() meaningful.
	void   set_instance_cost_ms(double ms) { instance_cost_ms_ = ms < 0.0 ? 0.0 : ms; }
	double instance_cost_ms() const { return instance_cost_ms_; }
	// The streaming-attributable cost of the LAST tick: instancings × per-instancing
	// cost. Compare against kHitchGateMs to check the hitch gate.
	double last_stream_frame_cost_ms() const {
		return static_cast<double>(last_instanced_) * instance_cost_ms_;
	}
	// The largest instancing budget that keeps one tick within `gate_ms` at the given
	// per-instancing cost — the budget a caller should pass to tick() to honour the
	// gate. cost ≤ 0 ⇒ unbounded (returns INT_MAX); otherwise floor(gate/cost), ≥ 1.
	static int budget_for_hitch_gate(double instance_cost_ms, double gate_ms = kHitchGateMs) {
		if (instance_cost_ms <= 0.0) return std::numeric_limits<int>::max();
		const int b = static_cast<int>(gate_ms / instance_cost_ms);
		return b < 1 ? 1 : b;
	}

	// ── Queries (drive the headless verify) ──────────────────────────────────
	int  player_cx() const { return pcx_; }
	int  player_cz() const { return pcz_; }
	bool has_player() const { return has_player_; }

	std::size_t chunk_count() const { return chunks_.size(); }
	// Derived lifecycle state (Unloaded if not in zone). A chunk that is SHOWING a
	// representation reports Instanced even if a swap load is in flight behind it.
	ChunkState  state_at(int cx, int cz) const;
	int         chunk_id_at(int cx, int cz) const;          // -1 if not in zone

	// The representation currently INSTANCED (visible) for a chunk — Rep::None if
	// nothing is shown. Rep::Full inside the radius, Rep::Proxy in the far-ring band.
	Rep shown_rep_at(int cx, int cz) const;
	// The representation the chunk WANTS this frame given the player cell + tier
	// (Full inside radius, Proxy in the band iff it has one, else None). Drives the
	// swap and the far-ring verify.
	Rep desired_rep_at(int cx, int cz) const;

	// A read-only view of one managed chunk by id (0..chunk_count()-1) — coord,
	// priority, derived state, and the shown/loading representations — so a caller
	// (the wrapper's cell queries, a HUD) can enumerate chunks without re-deriving
	// the grid.
	struct ChunkView {
		int cx = 0, cz = 0, priority = 0;
		ChunkState state = ChunkState::Unloaded;   // derived lifecycle
		Rep shown = Rep::None;                      // instanced (visible) representation
		Rep loading = Rep::None;                    // representation being loaded (None if idle)
	};
	ChunkView view(std::size_t id) const;

	// Is (cx,cz) inside the current FULL-detail desired ring (within active_radius of
	// the player cell AND present in the zone)?
	bool is_desired(int cx, int cz) const;
	// Is (cx,cz) inside the PROXY far-ring band (radius < cheb ≤ far_ring, present,
	// and the chunk has a proxy)?
	bool is_proxy_desired(int cx, int cz) const;

	// Counts by DERIVED state (across all managed chunks).
	std::size_t count_state(ChunkState s) const;
	std::size_t desired_count() const;         // full-detail desired (within active_radius)
	std::size_t proxy_desired_count() const;   // chunks wanting a proxy this frame
	std::size_t loading_count() const;         // slots with a load in flight (Loading)
	std::size_t ready_count() const;           // slots with a finished-but-uninstanced load
	std::size_t instanced_count() const;       // slots SHOWING any representation
	std::size_t proxy_instanced_count() const; // slots showing a PROXY
	std::size_t full_instanced_count() const;  // slots showing a FULL mesh
	// Resident = any chunk the streamer is actively holding (a load in flight OR a
	// representation shown) — i.e. consuming a slot toward a band.
	std::size_t resident_count() const;

	// The most recent tick's instancing count (for the budget assertion).
	int last_instanced_this_tick() const { return last_instanced_; }
	// Cumulative counters (diagnostics / verify).
	long long total_loads_requested() const { return total_loads_; }
	long long total_instanced() const       { return total_instanced_; }
	long long total_recycled() const        { return total_recycled_; }
	long long total_load_failures() const   { return total_failures_; }
	// Completed proxy↔full swaps (a new representation instanced over an old one).
	long long total_swaps() const           { return total_swaps_; }

private:
	// One managed chunk. Story C splits the single Story-B state into two axes so a
	// chunk can hold a visible representation AND load its swap target at once:
	//   * shown       — the representation currently instanced (None/Proxy/Full);
	//   * loading      — the representation being loaded toward (None when idle), with
	//   * load_state   — its lifecycle (Loading/Ready/Failed; Unloaded when idle).
	struct Slot {
		StreamChunk chunk;
		Rep shown = Rep::None;
		Rep loading = Rep::None;
		ChunkState load_state = ChunkState::Unloaded;
	};

	// Derived lifecycle for a slot (Instanced if anything is shown, else the load's
	// state, else Unloaded).
	static ChunkState derived_state(const Slot &s);
	// The res:// path for a representation of a chunk (Full → scene, Proxy → proxy).
	static const std::string &path_for(const StreamChunk &c, Rep rep);

	// Chebyshev distance of (cx,cz) from the current player cell.
	int chebyshev(int cx, int cz) const;
	// Chebyshev ring test around the current player cell (within the FULL radius).
	bool within_ring(int cx, int cz) const;
	// The representation a chunk wants this frame (Full ≤ radius; Proxy in the band
	// iff it has one; else None). Rep::None whenever there is no player.
	Rep desired_rep(const StreamChunk &c) const;

	IStreamBackend *backend_ = nullptr;
	StreamZone      zone_;
	std::vector<Slot> chunks_;
	TierRadiiConfig radii_;
	Tier            tier_ = Tier::Medium;

	bool has_player_ = false;
	int  pcx_ = 0, pcz_ = 0;

	int  last_instanced_ = 0;
	double instance_cost_ms_ = 0.0;   // modelled per-instancing main-thread cost (hitch gate)
	long long total_loads_ = 0;
	long long total_instanced_ = 0;
	long long total_recycled_ = 0;
	long long total_failures_ = 0;
	long long total_swaps_ = 0;
};

// Human-readable state / representation names (logs / test diagnostics).
const char *chunk_state_name(ChunkState s);
const char *rep_name(Rep r);

}  // namespace meridian::stream

#endif  // MERIDIAN_CHUNK_STREAM_CORE_H
