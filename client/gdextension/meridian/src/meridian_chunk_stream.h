// Project Meridian — MeridianChunkStream GDExtension node (issue #555, Epic #22
// Story B). The thin Godot binding over the engine-free chunk streamer core
// (chunk_stream_core.*): it owns the resident chunk instances as its own children,
// implements the core's IStreamBackend over Godot's ResourceLoader threaded-load,
// PackedScene instancing, and a per-chunk node pool, and exposes the streamer's
// set-player-pos → tick → query API to GDScript so the world scene (Story E, #558)
// can drive it and a headless verify can prove it.
//
// SPLIT OF RESPONSIBILITY (Client SAD §9.2, like MeridianTpsCamera over
// tps_camera_core): ALL streaming POLICY — world→cell math, the desired ring per
// tier radius, priority-ordered load dispatch, the per-frame instancing budget,
// the pooled unload — lives in the tested engine-free core. This node ONLY:
//   * builds the StreamZone the core drives (either from a GDScript-supplied
//     Dictionary, or — the production path — by reading the IF-6 chunks manifest +
//     IF-8 asset table and REUSING Story A's chunkpack::build_and_resolve to
//     resolve every chunk's baked-mesh scene res:// path; it does NOT re-parse),
//   * executes the engine primitives the core asks for (load_threaded_request /
//     _get_status / _get, instantiate + deferred add_child, deferred recycle into a
//     pool), and
//   * marshals the core's queries into Godot types (Vector2i cells, ints).
//
// This node extends Node3D because the instanced baked-mesh chunks (Q2 — baked
// `.scn`, no runtime Terrain3D) are added as its children in world space.

#ifndef MERIDIAN_CHUNK_STREAM_NODE_H
#define MERIDIAN_CHUNK_STREAM_NODE_H

#include "chunk_stream_core.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <unordered_map>

namespace meridian {

class MeridianChunkStream : public godot::Node3D {
	GDCLASS(MeridianChunkStream, godot::Node3D)

public:
	// GDScript-facing mirror of stream::ChunkState (same ordinals) so scenes/tests
	// can branch on `s == MeridianChunkStream.STATE_INSTANCED` without a magic int.
	enum ChunkStateEnum {
		STATE_UNLOADED  = 0,
		STATE_LOADING   = 1,
		STATE_READY     = 2,
		STATE_INSTANCED = 3,
		STATE_FAILED    = 4,
	};

	// GDScript-facing mirror of stream::Tier (== settings QualityTier ordinals).
	enum TierEnum {
		TIER_LOW    = 0,
		TIER_MEDIUM = 1,
		TIER_HIGH   = 2,
		TIER_EPIC   = 3,
	};

	// GDScript-facing mirror of stream::Rep (Story C): which representation of a
	// chunk is shown/desired — nothing, the low-poly proxy, or the full mesh.
	enum RepEnum {
		REP_NONE  = 0,
		REP_PROXY = 1,
		REP_FULL  = 2,
	};

protected:
	static void _bind_methods();

public:
	MeridianChunkStream();
	~MeridianChunkStream();

	// Godot lifecycle. When auto_tick is enabled, drives one streamer tick per
	// physics frame (Story E wiring); default OFF so a verify ticks manually.
	void _physics_process(double delta) override;

	// ── Zone configuration ────────────────────────────────────────────────────
	// PRODUCTION path: read the IF-6 chunks manifest (`<zone>.chunks.json`) + the
	// IF-8 asset table (`<zone>.assets.json`) via FileAccess and REUSE Story A's
	// chunkpack::build_and_resolve to resolve every chunk's baked-mesh scene res://
	// path, then install the resulting zone. Returns true on a clean parse+resolve.
	// (Presence/integrity is Story A's gate — call MeridianPackMount.verify_chunk_index
	// first; this only needs the resolved index to stream.)
	bool load_zone(const godot::String &chunks_json_path, const godot::String &assets_json_path);

	// LOW-LEVEL path: install a zone straight from a Dictionary the caller already
	// has in hand — { "origin_x": float, "origin_z": float, "chunk_size_m": int,
	// "min_cx"/"min_cz"/"max_cx"/"max_cz": int, "chunks": Array[ { "cx": int,
	// "cz": int, "priority": int, "scene": String(res://…), "proxy": String(res://…)
	// or "" for no proxy } ] }. Used by the headless verify (synthetic zone over
	// user:// scenes) and any caller driving the streamer from an already-parsed
	// index. An absent/empty "proxy" means the far-ring band shows nothing (C3).
	void configure(const godot::Dictionary &zone);

	// ── Tier / radius config (Q3) ─────────────────────────────────────────────
	void set_tier(int tier);
	int  get_tier() const;
	// Override one tier's full-detail ring radius (rings). Q3 defaults: Low 1 /
	// Medium 2 / Epic 3 (High 3). Config, not a constant.
	void set_tier_radius(int tier, int rings);
	int  get_tier_radius(int tier) const;
	int  get_active_radius() const;
	// Override one tier's PROXY far-ring radius (rings). Q3 defaults: Low 3 /
	// Medium 4 / Epic 6 (High 6). Config, not a constant (Story C).
	void set_tier_far_ring(int tier, int rings);
	int  get_tier_far_ring(int tier) const;
	int  get_active_far_ring() const;

	// The per-frame instancing budget (time-slice). Used by _physics_process auto
	// ticks and as the default for tick(). Default 2.
	void set_instancing_budget(int budget);
	int  get_instancing_budget() const;

	void set_auto_tick(bool enabled);
	bool get_auto_tick() const;

	// ── Hitch gate (Story C, Client SAD §3d: ≤ 50 ms streaming per frame) ──────
	// Model the per-instancing main-thread cost (ms) used to report the streaming-
	// attributable frame cost. Real tier-machine numbers land via the perf fleet
	// (#31); default 0 (cost reporting off).
	void   set_instance_cost_ms(double ms);
	double get_instance_cost_ms() const;
	// The streaming cost of the last tick (instancings × per-instancing cost).
	double get_last_stream_frame_cost_ms() const;
	// The largest instancing budget that keeps one tick within the 50 ms gate at the
	// given per-instancing cost — pass to set_instancing_budget() to honour the gate.
	int    budget_for_hitch_gate(double instance_cost_ms) const;

	// ── Drive ─────────────────────────────────────────────────────────────────
	// Update the tracked player position (metres, XZ from a Vector3).
	void set_player_position(const godot::Vector3 &world_pos);
	// Advance one streamer tick using the configured instancing budget. Returns the
	// number of chunks instanced this tick (≤ budget).
	int tick();
	// Advance one tick with an explicit budget (overrides the configured one).
	int tick_budget(int budget);

	// ── Queries (drive the headless verify / HUD) ─────────────────────────────
	godot::Vector2i world_to_cell(const godot::Vector3 &world_pos) const;
	godot::Vector2i get_player_cell() const;
	int  state_at(int cx, int cz) const;               // ChunkStateEnum
	bool is_desired(int cx, int cz) const;
	// Story C: the representation SHOWN / DESIRED for a chunk (RepEnum), and whether
	// a chunk is in the proxy far-ring band this frame.
	int  shown_rep_at(int cx, int cz) const;           // RepEnum
	int  desired_rep_at(int cx, int cz) const;         // RepEnum
	bool is_proxy_desired(int cx, int cz) const;

	int get_chunk_count() const;
	int get_desired_count() const;
	int get_proxy_desired_count() const;
	int get_loading_count() const;
	int get_ready_count() const;
	int get_instanced_count() const;
	int get_proxy_instanced_count() const;
	int get_full_instanced_count() const;
	int get_resident_count() const;
	int get_last_instanced_this_tick() const;

	godot::TypedArray<godot::Vector2i> get_desired_cells() const;
	godot::TypedArray<godot::Vector2i> get_loading_cells() const;
	godot::TypedArray<godot::Vector2i> get_instanced_cells() const;
	godot::TypedArray<godot::Vector2i> get_proxy_instanced_cells() const;
	godot::TypedArray<godot::Vector2i> get_full_instanced_cells() const;

	// Pool diagnostics (proves recycle, not churn-free): how many detached instances
	// sit in the pool, how many times an instance was reused from it, how many chunks
	// were recycled in total, and how many proxy↔full swaps have completed.
	int get_pool_size() const;
	int get_pool_reuse_count() const;
	int get_recycle_count() const;
	int get_swap_count() const;

	// Cumulative core counters (diagnostics).
	int get_total_loads_requested() const;
	int get_total_load_failures() const;

private:
	// The Godot-backed implementation of the core's engine seam. Forwards each
	// primitive to the owning node (ResourceLoader threaded load, PackedScene
	// instancing, the per-(chunk,representation) pool). Story C: each call carries
	// the representation so proxy and full nodes pool separately.
	struct GodotBackend final : public stream::IStreamBackend {
		MeridianChunkStream *owner = nullptr;
		void request_load(int chunk_id, stream::Rep rep, const std::string &scene_path) override;
		stream::LoadPoll poll_load(int chunk_id, stream::Rep rep, const std::string &scene_path) override;
		void instantiate(int chunk_id, stream::Rep rep, const std::string &scene_path) override;
		void recycle(int chunk_id, stream::Rep rep) override;
		void release_load(int chunk_id, stream::Rep rep, const std::string &scene_path) override;
	};
	friend struct GodotBackend;

	// Compose a stable per-(chunk,representation) key for the instance / pool maps —
	// a proxy and a full for the same chunk_id must never collide (a proxy recycled
	// for a chunk must not be reused as its full mesh).
	static long inst_key(int chunk_id, stream::Rep rep) {
		return static_cast<long>(chunk_id) * 3 + static_cast<int>(rep);
	}

	// Free the detached pooled instances (memdelete) — used at destruction (Godot's
	// Node free already cascades to the attached child instances) and by reset_streamed.
	void free_pool();
	// Drop the current zone's instances (queue_free the attached, free the pooled) and
	// reset the pool counters — used on (re)configure while this node is alive.
	void reset_streamed();

	stream::ChunkStreamer streamer_;
	GodotBackend          backend_;

	int  budget_ = 2;
	bool auto_tick_ = false;

	// Chunk instances currently attached under this node, and the recycle pool of
	// detached instances — both keyed by inst_key(chunk_id, rep) so proxy and full
	// nodes are tracked and pooled separately (Story C). Stored as Node* so a scene
	// root of any Node subtype is handled (the baked chunk scenes root a
	// MeshInstance3D, but nothing here assumes it).
	std::unordered_map<long, godot::Node *> instances_;
	std::unordered_map<long, godot::Node *> pool_;

	int pool_reuse_count_ = 0;
	int recycle_count_ = 0;
};

} // namespace meridian

VARIANT_ENUM_CAST(meridian::MeridianChunkStream::ChunkStateEnum);
VARIANT_ENUM_CAST(meridian::MeridianChunkStream::TierEnum);
VARIANT_ENUM_CAST(meridian::MeridianChunkStream::RepEnum);

#endif // MERIDIAN_CHUNK_STREAM_NODE_H
