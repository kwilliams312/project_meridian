// Project Meridian — MeridianChunkStream GDExtension node (issue #555, Epic #22
// Story B). See meridian_chunk_stream.h for the contract. This file is the Godot
// glue: it implements the core's IStreamBackend over ResourceLoader's threaded
// load + PackedScene instancing + a per-chunk node pool, builds the StreamZone the
// core drives (from a Dictionary or by reusing Story A's chunkpack resolver), and
// marshals the core's queries into Godot types.

#include "meridian_chunk_stream.h"

#include "chunk_pack_core.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

using namespace godot;

namespace meridian {

namespace {
// Godot String (UTF-8) -> std::string for the engine-free core.
std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}
} // namespace

// ── The Godot-backed engine seam ─────────────────────────────────────────────
void MeridianChunkStream::GodotBackend::request_load(int, const std::string &scene_path) {
	ResourceLoader::get_singleton()->load_threaded_request(String::utf8(scene_path.c_str()));
}

stream::LoadPoll MeridianChunkStream::GodotBackend::poll_load(int, const std::string &scene_path) {
	const ResourceLoader::ThreadLoadStatus st =
			ResourceLoader::get_singleton()->load_threaded_get_status(String::utf8(scene_path.c_str()));
	switch (st) {
		case ResourceLoader::THREAD_LOAD_LOADED:
			return stream::LoadPoll::Ready;
		case ResourceLoader::THREAD_LOAD_IN_PROGRESS:
			return stream::LoadPoll::InProgress;
		case ResourceLoader::THREAD_LOAD_FAILED:
		case ResourceLoader::THREAD_LOAD_INVALID_RESOURCE:
		default:
			return stream::LoadPoll::Failed;
	}
}

void MeridianChunkStream::GodotBackend::instantiate(int chunk_id, const std::string &scene_path) {
	const String p = String::utf8(scene_path.c_str());

	// Re-attach a recycled instance for this chunk if one is pooled — the whole
	// point of pooled unload: re-entering a just-left chunk skips instantiation.
	auto pooled = owner->pool_.find(chunk_id);
	if (pooled != owner->pool_.end()) {
		// The core (which does not know the backend pooled this chunk) issued a
		// fresh load_threaded_request for it on re-entry; DRAIN it so the completed
		// resource is not orphaned in the loader's threaded-load cache — then reuse
		// the pooled node instead of instancing anew.
		ResourceLoader::get_singleton()->load_threaded_get(p);
		Node *n = pooled->second;
		owner->pool_.erase(pooled);
		owner->instances_[chunk_id] = n;
		owner->call_deferred("add_child", n);
		++owner->pool_reuse_count_;
		return;
	}

	Ref<PackedScene> ps = ResourceLoader::get_singleton()->load_threaded_get(p);
	if (ps.is_null()) {
		// Load did not yield a PackedScene — surface and skip (state stays as the
		// core set it; a re-tick will not re-instance a non-resident chunk).
		UtilityFunctions::push_warning("MeridianChunkStream: load yielded no PackedScene: " + p);
		return;
	}
	Node *inst = ps->instantiate();
	if (inst == nullptr) {
		UtilityFunctions::push_warning("MeridianChunkStream: instantiate() returned null: " + p);
		return;
	}
	owner->instances_[chunk_id] = inst;
	owner->call_deferred("add_child", inst);
}

void MeridianChunkStream::GodotBackend::recycle(int chunk_id) {
	auto it = owner->instances_.find(chunk_id);
	if (it == owner->instances_.end()) {
		return;
	}
	Node *n = it->second;
	owner->instances_.erase(it);
	if (n != nullptr) {
		// Detach but KEEP the node — pooled, not freed (recycle, don't churn-free).
		owner->call_deferred("remove_child", n);
		owner->pool_[chunk_id] = n;
		++owner->recycle_count_;
	}
}

void MeridianChunkStream::GodotBackend::release_load(int, const std::string &) {
	// A requested/ready-but-never-instanced load. Godot's ResourceLoader has no
	// cancel; the load simply completes into the resource cache (bounded — a chunk
	// pack is small). Nothing attached to the tree, so nothing to detach here.
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
MeridianChunkStream::MeridianChunkStream() {
	backend_.owner = this;
	streamer_.set_backend(&backend_);
}

MeridianChunkStream::~MeridianChunkStream() {
	// Attached instances are CHILDREN — Godot's Node free cascades to them, so we do
	// NOT free them here (that would double-free). Only the POOLED instances are
	// detached from the tree and must be freed explicitly.
	free_pool();
}

void MeridianChunkStream::free_pool() {
	for (auto &kv : pool_) {
		if (kv.second != nullptr) {
			memdelete(kv.second);
		}
	}
	pool_.clear();
}

void MeridianChunkStream::reset_streamed() {
	// Called on (re)configure while this node is ALIVE (not being freed): drop the
	// prior zone's instances so a new zone starts clean. Attached instances are
	// queue_free'd (safe whether or not in the tree); pooled ones are freed now.
	for (auto &kv : instances_) {
		if (kv.second != nullptr) {
			kv.second->queue_free();
		}
	}
	instances_.clear();
	free_pool();
	pool_reuse_count_ = 0;
	recycle_count_ = 0;
}

void MeridianChunkStream::_physics_process(double) {
	if (auto_tick_) {
		streamer_.tick(budget_);
	}
}

// ── Zone configuration ───────────────────────────────────────────────────────
bool MeridianChunkStream::load_zone(const String &chunks_json_path, const String &assets_json_path) {
	if (!FileAccess::file_exists(chunks_json_path) || !FileAccess::file_exists(assets_json_path)) {
		UtilityFunctions::push_error("MeridianChunkStream.load_zone: manifest / asset table not found");
		return false;
	}
	Ref<FileAccess> cf = FileAccess::open(chunks_json_path, FileAccess::READ);
	Ref<FileAccess> af = FileAccess::open(assets_json_path, FileAccess::READ);
	if (cf.is_null() || af.is_null()) {
		UtilityFunctions::push_error("MeridianChunkStream.load_zone: could not open manifest / asset table");
		return false;
	}
	const std::string chunks_json = to_std(cf->get_as_text());
	const std::string assets_json = to_std(af->get_as_text());

	// REUSE Story A's parse+resolve (no disk): resolves every chunk's baked-mesh
	// scene res:// path off the IF-8 table. We do NOT re-parse the manifest.
	const chunkpack::ChunkPackReport rep = chunkpack::build_and_resolve(chunks_json, assets_json);
	if (rep.verdict != chunkpack::ChunkPackVerdict::kOk) {
		UtilityFunctions::push_error(String("MeridianChunkStream.load_zone: chunk index did not resolve: ") +
				String::utf8(rep.reason.c_str()));
		return false;
	}

	const chunkpack::ChunkIndex &idx = rep.index;
	stream::StreamZone z;
	z.origin_x = idx.origin.x;
	z.origin_z = idx.origin.z;
	z.chunk_size_m = idx.chunk_size_m;
	z.min_cx = idx.min_cx;
	z.min_cz = idx.min_cz;
	z.max_cx = idx.max_cx;
	z.max_cz = idx.max_cz;
	z.chunks.reserve(idx.chunks.size());
	for (const chunkpack::ResolvedChunk &rc : idx.chunks) {
		stream::StreamChunk sc;
		sc.cx = rc.cx;
		sc.cz = rc.cz;
		sc.priority = rc.priority;   // 0 when absent — treated as most urgent
		sc.scene_path = rc.scene.res_path;
		z.chunks.push_back(std::move(sc));
	}
	reset_streamed();   // drop any prior zone's instances/pool before re-configuring
	streamer_.configure(z);
	return true;
}

void MeridianChunkStream::configure(const Dictionary &zone) {
	auto as_int = [](const Variant &v) { return static_cast<int>(static_cast<int64_t>(v)); };

	stream::StreamZone z;
	z.origin_x = static_cast<double>(zone.get("origin_x", 0.0));
	z.origin_z = static_cast<double>(zone.get("origin_z", 0.0));
	z.chunk_size_m = as_int(zone.get("chunk_size_m", 0));
	z.min_cx = as_int(zone.get("min_cx", 0));
	z.min_cz = as_int(zone.get("min_cz", 0));
	z.max_cx = as_int(zone.get("max_cx", 0));
	z.max_cz = as_int(zone.get("max_cz", 0));

	const Array chunks = zone.get("chunks", Array());
	z.chunks.reserve(static_cast<std::size_t>(chunks.size()));
	for (int i = 0; i < chunks.size(); ++i) {
		const Dictionary c = chunks[i];
		stream::StreamChunk sc;
		sc.cx = as_int(c.get("cx", 0));
		sc.cz = as_int(c.get("cz", 0));
		sc.priority = as_int(c.get("priority", 0));
		sc.scene_path = to_std(String(c.get("scene", String())));
		z.chunks.push_back(std::move(sc));
	}
	reset_streamed();   // drop any prior zone's instances/pool before re-configuring
	streamer_.configure(z);
}

// ── Tier / radius config ─────────────────────────────────────────────────────
void MeridianChunkStream::set_tier(int tier) {
	streamer_.set_tier(static_cast<stream::Tier>(tier));
}
int MeridianChunkStream::get_tier() const {
	return static_cast<int>(streamer_.tier());
}
void MeridianChunkStream::set_tier_radius(int tier, int rings) {
	streamer_.set_tier_radius(static_cast<stream::Tier>(tier), rings);
}
int MeridianChunkStream::get_tier_radius(int tier) const {
	return streamer_.tier_radii().radius_for(static_cast<stream::Tier>(tier));
}
int MeridianChunkStream::get_active_radius() const {
	return streamer_.active_radius();
}

void MeridianChunkStream::set_instancing_budget(int budget) { budget_ = budget; }
int  MeridianChunkStream::get_instancing_budget() const { return budget_; }
void MeridianChunkStream::set_auto_tick(bool enabled) { auto_tick_ = enabled; }
bool MeridianChunkStream::get_auto_tick() const { return auto_tick_; }

// ── Drive ────────────────────────────────────────────────────────────────────
void MeridianChunkStream::set_player_position(const Vector3 &world_pos) {
	streamer_.set_player_position(world_pos.x, world_pos.z);
}
int MeridianChunkStream::tick() { return streamer_.tick(budget_); }
int MeridianChunkStream::tick_budget(int budget) { return streamer_.tick(budget); }

// ── Queries ──────────────────────────────────────────────────────────────────
Vector2i MeridianChunkStream::world_to_cell(const Vector3 &world_pos) const {
	int cx = 0, cz = 0;
	streamer_.world_to_cell(world_pos.x, world_pos.z, cx, cz);
	return Vector2i(cx, cz);
}
Vector2i MeridianChunkStream::get_player_cell() const {
	return Vector2i(streamer_.player_cx(), streamer_.player_cz());
}
int MeridianChunkStream::state_at(int cx, int cz) const {
	return static_cast<int>(streamer_.state_at(cx, cz));
}
bool MeridianChunkStream::is_desired(int cx, int cz) const {
	return streamer_.is_desired(cx, cz);
}

int MeridianChunkStream::get_chunk_count() const { return static_cast<int>(streamer_.chunk_count()); }
int MeridianChunkStream::get_desired_count() const { return static_cast<int>(streamer_.desired_count()); }
int MeridianChunkStream::get_loading_count() const { return static_cast<int>(streamer_.loading_count()); }
int MeridianChunkStream::get_ready_count() const { return static_cast<int>(streamer_.ready_count()); }
int MeridianChunkStream::get_instanced_count() const { return static_cast<int>(streamer_.instanced_count()); }
int MeridianChunkStream::get_resident_count() const { return static_cast<int>(streamer_.resident_count()); }
int MeridianChunkStream::get_last_instanced_this_tick() const { return streamer_.last_instanced_this_tick(); }

TypedArray<Vector2i> MeridianChunkStream::get_desired_cells() const {
	TypedArray<Vector2i> out;
	for (std::size_t i = 0; i < streamer_.chunk_count(); ++i) {
		const stream::ChunkStreamer::ChunkView v = streamer_.view(i);
		if (streamer_.is_desired(v.cx, v.cz)) {
			out.push_back(Vector2i(v.cx, v.cz));
		}
	}
	return out;
}
TypedArray<Vector2i> MeridianChunkStream::get_loading_cells() const {
	TypedArray<Vector2i> out;
	for (std::size_t i = 0; i < streamer_.chunk_count(); ++i) {
		const stream::ChunkStreamer::ChunkView v = streamer_.view(i);
		if (v.state == stream::ChunkState::Loading) {
			out.push_back(Vector2i(v.cx, v.cz));
		}
	}
	return out;
}
TypedArray<Vector2i> MeridianChunkStream::get_instanced_cells() const {
	TypedArray<Vector2i> out;
	for (std::size_t i = 0; i < streamer_.chunk_count(); ++i) {
		const stream::ChunkStreamer::ChunkView v = streamer_.view(i);
		if (v.state == stream::ChunkState::Instanced) {
			out.push_back(Vector2i(v.cx, v.cz));
		}
	}
	return out;
}

int MeridianChunkStream::get_pool_size() const { return static_cast<int>(pool_.size()); }
int MeridianChunkStream::get_pool_reuse_count() const { return pool_reuse_count_; }
int MeridianChunkStream::get_recycle_count() const { return recycle_count_; }
int MeridianChunkStream::get_total_loads_requested() const {
	return static_cast<int>(streamer_.total_loads_requested());
}
int MeridianChunkStream::get_total_load_failures() const {
	return static_cast<int>(streamer_.total_load_failures());
}

// ── Binding ──────────────────────────────────────────────────────────────────
void MeridianChunkStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_zone", "chunks_json_path", "assets_json_path"),
			&MeridianChunkStream::load_zone);
	ClassDB::bind_method(D_METHOD("configure", "zone"), &MeridianChunkStream::configure);

	ClassDB::bind_method(D_METHOD("set_tier", "tier"), &MeridianChunkStream::set_tier);
	ClassDB::bind_method(D_METHOD("get_tier"), &MeridianChunkStream::get_tier);
	ClassDB::bind_method(D_METHOD("set_tier_radius", "tier", "rings"), &MeridianChunkStream::set_tier_radius);
	ClassDB::bind_method(D_METHOD("get_tier_radius", "tier"), &MeridianChunkStream::get_tier_radius);
	ClassDB::bind_method(D_METHOD("get_active_radius"), &MeridianChunkStream::get_active_radius);

	ClassDB::bind_method(D_METHOD("set_instancing_budget", "budget"), &MeridianChunkStream::set_instancing_budget);
	ClassDB::bind_method(D_METHOD("get_instancing_budget"), &MeridianChunkStream::get_instancing_budget);
	ClassDB::bind_method(D_METHOD("set_auto_tick", "enabled"), &MeridianChunkStream::set_auto_tick);
	ClassDB::bind_method(D_METHOD("get_auto_tick"), &MeridianChunkStream::get_auto_tick);

	ClassDB::bind_method(D_METHOD("set_player_position", "world_pos"), &MeridianChunkStream::set_player_position);
	ClassDB::bind_method(D_METHOD("tick"), &MeridianChunkStream::tick);
	ClassDB::bind_method(D_METHOD("tick_budget", "budget"), &MeridianChunkStream::tick_budget);

	ClassDB::bind_method(D_METHOD("world_to_cell", "world_pos"), &MeridianChunkStream::world_to_cell);
	ClassDB::bind_method(D_METHOD("get_player_cell"), &MeridianChunkStream::get_player_cell);
	ClassDB::bind_method(D_METHOD("state_at", "cx", "cz"), &MeridianChunkStream::state_at);
	ClassDB::bind_method(D_METHOD("is_desired", "cx", "cz"), &MeridianChunkStream::is_desired);

	ClassDB::bind_method(D_METHOD("get_chunk_count"), &MeridianChunkStream::get_chunk_count);
	ClassDB::bind_method(D_METHOD("get_desired_count"), &MeridianChunkStream::get_desired_count);
	ClassDB::bind_method(D_METHOD("get_loading_count"), &MeridianChunkStream::get_loading_count);
	ClassDB::bind_method(D_METHOD("get_ready_count"), &MeridianChunkStream::get_ready_count);
	ClassDB::bind_method(D_METHOD("get_instanced_count"), &MeridianChunkStream::get_instanced_count);
	ClassDB::bind_method(D_METHOD("get_resident_count"), &MeridianChunkStream::get_resident_count);
	ClassDB::bind_method(D_METHOD("get_last_instanced_this_tick"), &MeridianChunkStream::get_last_instanced_this_tick);

	ClassDB::bind_method(D_METHOD("get_desired_cells"), &MeridianChunkStream::get_desired_cells);
	ClassDB::bind_method(D_METHOD("get_loading_cells"), &MeridianChunkStream::get_loading_cells);
	ClassDB::bind_method(D_METHOD("get_instanced_cells"), &MeridianChunkStream::get_instanced_cells);

	ClassDB::bind_method(D_METHOD("get_pool_size"), &MeridianChunkStream::get_pool_size);
	ClassDB::bind_method(D_METHOD("get_pool_reuse_count"), &MeridianChunkStream::get_pool_reuse_count);
	ClassDB::bind_method(D_METHOD("get_recycle_count"), &MeridianChunkStream::get_recycle_count);
	ClassDB::bind_method(D_METHOD("get_total_loads_requested"), &MeridianChunkStream::get_total_loads_requested);
	ClassDB::bind_method(D_METHOD("get_total_load_failures"), &MeridianChunkStream::get_total_load_failures);

	// Editor / GDScript properties.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "instancing_budget"),
			"set_instancing_budget", "get_instancing_budget");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_tick"), "set_auto_tick", "get_auto_tick");

	BIND_ENUM_CONSTANT(STATE_UNLOADED);
	BIND_ENUM_CONSTANT(STATE_LOADING);
	BIND_ENUM_CONSTANT(STATE_READY);
	BIND_ENUM_CONSTANT(STATE_INSTANCED);
	BIND_ENUM_CONSTANT(STATE_FAILED);

	BIND_ENUM_CONSTANT(TIER_LOW);
	BIND_ENUM_CONSTANT(TIER_MEDIUM);
	BIND_ENUM_CONSTANT(TIER_HIGH);
	BIND_ENUM_CONSTANT(TIER_EPIC);
}

} // namespace meridian
