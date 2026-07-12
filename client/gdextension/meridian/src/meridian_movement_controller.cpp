// Project Meridian — MeridianMovementController GDExtension node (issue #102).
// Thin Godot glue over the engine-free core (movement_controller.*).

#include "meridian_movement_controller.h"

#include "heightfield_chunk_decode.h"

#include <godot_cpp/core/class_db.hpp>

#include <utility>

using namespace godot;

namespace meridian {

namespace mv = meridian::movement;

MeridianMovementController::MeridianMovementController()
    : world_(std::make_unique<mv::FlatWorldQuery>(0.0f)) {  // M0 flat plane (D-19)
	ensure_reconciler();
}

MeridianMovementController::~MeridianMovementController() {}

void MeridianMovementController::ensure_reconciler() {
	if (!reconciler_) {
		mv::MovementSnapshot start;  // origin, grounded on the y=0 flat map (D-19)
		reconciler_ = std::make_unique<mv::PredictionReconciler>(*world_, start);
	}
}

void MeridianMovementController::_bind_methods() {
	ClassDB::bind_method(D_METHOD("reset", "position", "orientation"),
	                     &MeridianMovementController::reset);
	ClassDB::bind_method(
	    D_METHOD("predict", "move", "walk", "jump", "orientation", "client_time_ms"),
	    &MeridianMovementController::predict);
	ClassDB::bind_method(
	    D_METHOD("reconcile", "ack_seq", "server_position", "orientation"),
	    &MeridianMovementController::reconcile);
	ClassDB::bind_method(D_METHOD("should_emit_intent", "client_time_ms", "state_flags"),
	                     &MeridianMovementController::should_emit_intent);
	ClassDB::bind_method(D_METHOD("advance_smoothing", "dt_ms"),
	                     &MeridianMovementController::advance_smoothing);
	ClassDB::bind_method(D_METHOD("get_predicted_position"),
	                     &MeridianMovementController::get_predicted_position);
	ClassDB::bind_method(D_METHOD("get_render_position"),
	                     &MeridianMovementController::get_render_position);
	ClassDB::bind_method(D_METHOD("get_predicted_velocity"),
	                     &MeridianMovementController::get_predicted_velocity);
	ClassDB::bind_method(D_METHOD("is_grounded"),
	                     &MeridianMovementController::is_grounded);
	ClassDB::bind_method(D_METHOD("pending_input_count"),
	                     &MeridianMovementController::pending_input_count);
	ClassDB::bind_method(D_METHOD("is_smoothing"),
	                     &MeridianMovementController::is_smoothing);
	ClassDB::bind_method(D_METHOD("last_error_magnitude"),
	                     &MeridianMovementController::last_error_magnitude);
	ClassDB::bind_method(D_METHOD("last_reconcile_snapped"),
	                     &MeridianMovementController::last_reconcile_snapped);
	ClassDB::bind_method(
	    D_METHOD("use_heightfield_zone", "origin_x", "origin_z", "chunk_size_m"),
	    &MeridianMovementController::use_heightfield_zone);
	ClassDB::bind_method(D_METHOD("add_heightfield_chunk", "cx", "cz", "chunk_bin"),
	                     &MeridianMovementController::add_heightfield_chunk);
	ClassDB::bind_method(D_METHOD("sample_ground", "x", "z"),
	                     &MeridianMovementController::sample_ground);
	ClassDB::bind_method(D_METHOD("has_ground_at", "x", "z"),
	                     &MeridianMovementController::has_ground_at);
	ClassDB::bind_method(D_METHOD("is_heightfield_active"),
	                     &MeridianMovementController::is_heightfield_active);
	ClassDB::bind_method(D_METHOD("heightfield_chunk_count"),
	                     &MeridianMovementController::heightfield_chunk_count);
}

void MeridianMovementController::reset(const Vector3 &position, float orientation) {
	mv::MovementSnapshot start;
	start.position    = mv::Vec3{position.x, position.y, position.z};
	start.orientation = orientation;
	const mv::GroundSample g = world_->sample_ground(position.x, position.z);
	// Over ground that is not yet resident (walkable == false) we cannot know the
	// surface, so treat the seeded position as supported — never derive "airborne"
	// from an unknown ground and let the next tick integrate a fall (#558 no-fall-through).
	start.grounded = (!g.walkable) || (position.y <= g.height + 1e-4f);
	reconciler_ = std::make_unique<mv::PredictionReconciler>(*world_, start);
}

// ── M1 heightfield ground backend (#557 Story D → #558 Story E) ───────────────

void MeridianMovementController::use_heightfield_zone(float origin_x, float origin_z,
                                                      float chunk_size_m) {
	auto hf = std::make_unique<mv::HeightfieldWorldQuery>(origin_x, origin_z, chunk_size_m);
	heightfield_ = hf.get();                 // non-owning alias for add_heightfield_chunk
	// Carry the current predicted state across the backend swap so an in-flight
	// session keeps its position (the world scene re-seeds via reset() once the
	// spawn chunk is resident and it can sample the real ground height).
	mv::MovementSnapshot start;
	if (reconciler_) start = reconciler_->predicted_state();
	world_ = std::move(hf);                   // FlatWorldQuery is freed here
	reconciler_ = std::make_unique<mv::PredictionReconciler>(*world_, start);
}

bool MeridianMovementController::add_heightfield_chunk(int cx, int cz,
                                                       const PackedByteArray &chunk_bin) {
	if (heightfield_ == nullptr) return false;   // no heightfield zone active
	mv::HeightfieldChunk chunk;
	if (!mv::decode_heightfield_chunk(chunk_bin.ptr(),
	                                  static_cast<std::size_t>(chunk_bin.size()), chunk)) {
		return false;                            // fail-closed: never a silent bad grid
	}
	// Key the resident chunk by the manifest cell the caller resolved it from, so
	// find_chunk (world-XZ → cell) lines up with the IF-6 grid indexing exactly.
	chunk.cx = cx;
	chunk.cz = cz;
	heightfield_->add_chunk(std::move(chunk));
	return true;
}

Dictionary MeridianMovementController::sample_ground(float x, float z) const {
	Dictionary d;
	const mv::GroundSample g = world_->sample_ground(x, z);
	d["height"]   = g.height;
	d["walkable"] = g.walkable;
	return d;
}

bool MeridianMovementController::has_ground_at(float x, float z) const {
	return world_->sample_ground(x, z).walkable;
}

bool MeridianMovementController::is_heightfield_active() const {
	return heightfield_ != nullptr;
}

int MeridianMovementController::heightfield_chunk_count() const {
	return heightfield_ != nullptr ? static_cast<int>(heightfield_->chunk_count()) : 0;
}

Dictionary MeridianMovementController::predict(const Vector3 &move, bool walk,
                                               bool jump, float orientation,
                                               uint64_t client_time_ms) {
	ensure_reconciler();
	mv::MovementInput in;
	in.move_x      = move.x;
	in.move_z      = move.z;
	in.walk        = walk;
	in.jump        = jump;
	in.orientation = orientation;

	const mv::MovementIntentOut out = reconciler_->predict(in, client_time_ms);

	Dictionary d;
	d["seq"]            = out.seq;
	d["state_flags"]    = out.state_flags;
	d["x"]              = out.x;
	d["y"]              = out.y;
	d["z"]              = out.z;
	d["orientation"]    = out.orientation;
	d["client_time_ms"] = out.client_time_ms;
	return d;
}

Vector3 MeridianMovementController::reconcile(uint32_t ack_seq,
                                              const Vector3 &server_position,
                                              float orientation) {
	ensure_reconciler();
	mv::MovementStateIn s;
	s.ack_seq     = ack_seq;
	s.position    = mv::Vec3{server_position.x, server_position.y, server_position.z};
	s.orientation = orientation;
	const mv::MovementSnapshot r = reconciler_->reconcile(s);
	return Vector3(r.position.x, r.position.y, r.position.z);
}

bool MeridianMovementController::should_emit_intent(uint64_t client_time_ms,
                                                    uint32_t state_flags) {
	ensure_reconciler();
	return reconciler_->should_emit_intent(client_time_ms, state_flags);
}

void MeridianMovementController::advance_smoothing(uint64_t dt_ms) {
	if (reconciler_) reconciler_->advance_smoothing(dt_ms);
}

Vector3 MeridianMovementController::get_predicted_position() const {
	if (!reconciler_) return Vector3();
	const mv::Vec3 &p = reconciler_->predicted_state().position;
	return Vector3(p.x, p.y, p.z);
}

Vector3 MeridianMovementController::get_render_position() const {
	if (!reconciler_) return Vector3();
	const mv::Vec3 p = reconciler_->visible_state().position;
	return Vector3(p.x, p.y, p.z);
}

Vector3 MeridianMovementController::get_predicted_velocity() const {
	if (!reconciler_) return Vector3();
	const mv::Vec3 &v = reconciler_->predicted_state().velocity;
	return Vector3(v.x, v.y, v.z);
}

bool MeridianMovementController::is_grounded() const {
	return reconciler_ ? reconciler_->predicted_state().grounded : true;
}

uint32_t MeridianMovementController::pending_input_count() const {
	return reconciler_ ? static_cast<uint32_t>(reconciler_->pending_count()) : 0u;
}

bool MeridianMovementController::is_smoothing() const {
	return reconciler_ ? reconciler_->is_smoothing() : false;
}

float MeridianMovementController::last_error_magnitude() const {
	return reconciler_ ? reconciler_->last_error_magnitude() : 0.0f;
}

bool MeridianMovementController::last_reconcile_snapped() const {
	return reconciler_ ? reconciler_->last_reconcile_snapped() : false;
}

} // namespace meridian
