// Project Meridian — MeridianTpsCamera GDExtension node (issue #105).
// Thin Godot glue over the engine-free core (tps_camera_core.*). See the header
// for the rig layout + the C++-vs-GDScript rationale.

#include "meridian_tps_camera.h"

#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace meridian {

MeridianTpsCamera::MeridianTpsCamera() : core_(cfg_) {}

MeridianTpsCamera::~MeridianTpsCamera() {}

// ===========================================================================
// Binding
// ===========================================================================
void MeridianTpsCamera::_bind_methods() {
	// --- config properties ---
	ClassDB::bind_method(D_METHOD("set_yaw_sensitivity", "v"), &MeridianTpsCamera::set_yaw_sensitivity);
	ClassDB::bind_method(D_METHOD("get_yaw_sensitivity"), &MeridianTpsCamera::get_yaw_sensitivity);
	ClassDB::bind_method(D_METHOD("set_pitch_sensitivity", "v"), &MeridianTpsCamera::set_pitch_sensitivity);
	ClassDB::bind_method(D_METHOD("get_pitch_sensitivity"), &MeridianTpsCamera::get_pitch_sensitivity);
	ClassDB::bind_method(D_METHOD("set_pitch_min", "v"), &MeridianTpsCamera::set_pitch_min);
	ClassDB::bind_method(D_METHOD("get_pitch_min"), &MeridianTpsCamera::get_pitch_min);
	ClassDB::bind_method(D_METHOD("set_pitch_max", "v"), &MeridianTpsCamera::set_pitch_max);
	ClassDB::bind_method(D_METHOD("get_pitch_max"), &MeridianTpsCamera::get_pitch_max);
	ClassDB::bind_method(D_METHOD("set_zoom_min", "v"), &MeridianTpsCamera::set_zoom_min);
	ClassDB::bind_method(D_METHOD("get_zoom_min"), &MeridianTpsCamera::get_zoom_min);
	ClassDB::bind_method(D_METHOD("set_zoom_max", "v"), &MeridianTpsCamera::set_zoom_max);
	ClassDB::bind_method(D_METHOD("get_zoom_max"), &MeridianTpsCamera::get_zoom_max);
	ClassDB::bind_method(D_METHOD("set_zoom_step", "v"), &MeridianTpsCamera::set_zoom_step);
	ClassDB::bind_method(D_METHOD("get_zoom_step"), &MeridianTpsCamera::get_zoom_step);
	ClassDB::bind_method(D_METHOD("set_zoom_default", "v"), &MeridianTpsCamera::set_zoom_default);
	ClassDB::bind_method(D_METHOD("get_zoom_default"), &MeridianTpsCamera::get_zoom_default);
	ClassDB::bind_method(D_METHOD("set_zoom_spring", "v"), &MeridianTpsCamera::set_zoom_spring);
	ClassDB::bind_method(D_METHOD("get_zoom_spring"), &MeridianTpsCamera::get_zoom_spring);
	ClassDB::bind_method(D_METHOD("set_boom_spring", "v"), &MeridianTpsCamera::set_boom_spring);
	ClassDB::bind_method(D_METHOD("get_boom_spring"), &MeridianTpsCamera::get_boom_spring);
	ClassDB::bind_method(D_METHOD("set_camera_height", "v"), &MeridianTpsCamera::set_camera_height);
	ClassDB::bind_method(D_METHOD("get_camera_height"), &MeridianTpsCamera::get_camera_height);
	ClassDB::bind_method(D_METHOD("set_probe_radius", "v"), &MeridianTpsCamera::set_probe_radius);
	ClassDB::bind_method(D_METHOD("get_probe_radius"), &MeridianTpsCamera::get_probe_radius);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "v"), &MeridianTpsCamera::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &MeridianTpsCamera::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_capture_mouse", "v"), &MeridianTpsCamera::set_capture_mouse);
	ClassDB::bind_method(D_METHOD("get_capture_mouse"), &MeridianTpsCamera::get_capture_mouse);
	ClassDB::bind_method(D_METHOD("set_yaw_target_path", "p"), &MeridianTpsCamera::set_yaw_target_path);
	ClassDB::bind_method(D_METHOD("get_yaw_target_path"), &MeridianTpsCamera::get_yaw_target_path);

	ADD_GROUP("Sensitivity", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "yaw_sensitivity"), "set_yaw_sensitivity", "get_yaw_sensitivity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_sensitivity"), "set_pitch_sensitivity", "get_pitch_sensitivity");
	ADD_GROUP("Pitch Clamp", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_min"), "set_pitch_min", "get_pitch_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_max"), "set_pitch_max", "get_pitch_max");
	ADD_GROUP("Zoom", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "zoom_min"), "set_zoom_min", "get_zoom_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "zoom_max"), "set_zoom_max", "get_zoom_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "zoom_step"), "set_zoom_step", "get_zoom_step");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "zoom_default"), "set_zoom_default", "get_zoom_default");
	ADD_GROUP("Springs", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "zoom_spring"), "set_zoom_spring", "get_zoom_spring");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "boom_spring"), "set_boom_spring", "get_boom_spring");
	ADD_GROUP("Rig", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "camera_height"), "set_camera_height", "get_camera_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_radius"), "set_probe_radius", "get_probe_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS),
	             "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "capture_mouse"), "set_capture_mouse", "get_capture_mouse");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "yaw_target_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"),
	             "set_yaw_target_path", "get_yaw_target_path");

	// --- resolved-state accessors ---
	ClassDB::bind_method(D_METHOD("get_boom_length"), &MeridianTpsCamera::get_boom_length);
	ClassDB::bind_method(D_METHOD("get_camera_yaw"), &MeridianTpsCamera::get_camera_yaw);
	ClassDB::bind_method(D_METHOD("get_character_yaw"), &MeridianTpsCamera::get_character_yaw);
	ClassDB::bind_method(D_METHOD("get_pitch"), &MeridianTpsCamera::get_pitch);
	ClassDB::bind_method(D_METHOD("get_zoom"), &MeridianTpsCamera::get_zoom);

	// --- headless / test drivers ---
	ClassDB::bind_method(D_METHOD("feed_mouse_motion", "dx", "dy", "mode"), &MeridianTpsCamera::feed_mouse_motion);
	ClassDB::bind_method(D_METHOD("feed_zoom", "notches"), &MeridianTpsCamera::feed_zoom);
	ClassDB::bind_method(D_METHOD("step", "delta"), &MeridianTpsCamera::step);
	ClassDB::bind_method(D_METHOD("get_state"), &MeridianTpsCamera::get_state);
}

// ===========================================================================
// Lifecycle
// ===========================================================================
void MeridianTpsCamera::build_rig() {
	if (pivot_ != nullptr) {
		return;  // already built
	}
	// Reconstruct the core from the (possibly editor-tweaked) config so the
	// clamps + springs the properties describe are the ones the core enforces.
	core_ = camera::TpsCamera(cfg_);

	// PitchPivot — sits at head height, pitches about local X.
	pivot_ = memnew(Node3D);
	pivot_->set_name("PitchPivot");
	pivot_->set_position(Vector3(0.0f, camera_height_, 0.0f));
	add_child(pivot_);

	// SpringArm3D — a pure collision PROBE (no children). SpringArm3D casts toward
	// its local +Z, which is exactly where the camera sits (behind the character),
	// so the probe needs no rotation. We read get_hit_length() for the nearest
	// obstruction and feed it to the core; the SpringArm never moves the camera
	// itself (that keeps the smooth spring-out under our control).
	probe_ = memnew(SpringArm3D);
	probe_->set_name("BoomProbe");
	probe_->set_length(cfg_.zoom_max);
	probe_->set_collision_mask(collision_mask_);
	Ref<SphereShape3D> shape;
	shape.instantiate();
	shape->set_radius(probe_radius_);
	probe_->set_shape(shape);
	pivot_->add_child(probe_);

	// Exclude the player's own collider (nearest CollisionObject3D ancestor) so
	// the boom never collides with the character it is following.
	for (Node *n = get_parent(); n != nullptr; n = n->get_parent()) {
		CollisionObject3D *co = Object::cast_to<CollisionObject3D>(n);
		if (co != nullptr) {
			probe_->add_excluded_object(co->get_rid());
			break;
		}
	}

	// Camera3D — behind the pivot at +Z, looking down its own -Z (toward the
	// character and forward). Its distance is the resolved boom length.
	camera_ = memnew(Camera3D);
	camera_->set_name("Camera3D");
	camera_->set_position(Vector3(0.0f, 0.0f, core_.boom_length()));
	pivot_->add_child(camera_);
	if (!Engine::get_singleton()->is_editor_hint()) {
		camera_->set_current(true);
	}

	// Resolve the optional yaw target (the visible character body we rotate).
	yaw_target_ = nullptr;
	if (!yaw_target_path_.is_empty()) {
		yaw_target_ = Object::cast_to<Node3D>(get_node_or_null(yaw_target_path_));
	}

	apply_transforms();
}

void MeridianTpsCamera::_ready() {
	build_rig();
	if (Engine::get_singleton()->is_editor_hint()) {
		return;  // no input / physics driving at edit time
	}
	set_physics_process(true);
	set_process_unhandled_input(true);
}

void MeridianTpsCamera::_physics_process(double delta) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	step(delta);
}

camera::DragMode MeridianTpsCamera::current_drag_mode() const {
	if (rmb_held_) {
		return camera::DragMode::Steer;  // RMB wins if both are held (WoW)
	}
	if (lmb_held_) {
		return camera::DragMode::Orbit;
	}
	return camera::DragMode::None;
}

void MeridianTpsCamera::_unhandled_input(const Ref<InputEvent> &event) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(event.ptr());
	if (mb != nullptr) {
		const MouseButton btn = mb->get_button_index();
		const bool pressed = mb->is_pressed();
		if (btn == MOUSE_BUTTON_LEFT) {
			lmb_held_ = pressed;
			update_mouse_capture();
		} else if (btn == MOUSE_BUTTON_RIGHT) {
			rmb_held_ = pressed;
			update_mouse_capture();
		} else if (btn == MOUSE_BUTTON_WHEEL_UP && pressed) {
			core_.on_zoom(-1);  // wheel up = zoom IN (boom shorter)
		} else if (btn == MOUSE_BUTTON_WHEEL_DOWN && pressed) {
			core_.on_zoom(+1);  // wheel down = zoom OUT (boom longer)
		}
		return;
	}
	InputEventMouseMotion *mm = Object::cast_to<InputEventMouseMotion>(event.ptr());
	if (mm != nullptr) {
		const camera::DragMode mode = current_drag_mode();
		if (mode != camera::DragMode::None) {
			const Vector2 rel = mm->get_relative();
			core_.on_mouse_motion(rel.x, rel.y, mode);
		}
	}
}

void MeridianTpsCamera::update_mouse_capture() {
	if (!capture_mouse_ || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	Input *in = Input::get_singleton();
	const bool dragging = rmb_held_ || lmb_held_;
	in->set_mouse_mode(dragging ? Input::MOUSE_MODE_CAPTURED : Input::MOUSE_MODE_VISIBLE);
}

// ===========================================================================
// Transform application + collision query
// ===========================================================================
float MeridianTpsCamera::query_collision_distance() const {
	if (probe_ == nullptr) {
		return cfg_.zoom_max;
	}
	const float hit = static_cast<float>(probe_->get_hit_length());
	// A shape-cast never reports a hit closer than its probe radius, so a 0.0
	// reading is the SpringArm's not-yet-computed warm-up value (or the origin is
	// buried in geometry). Treat it as "no obstruction" so the boom doesn't snap
	// to the pivot on the first physics frame after (re)building the rig.
	if (hit <= 0.0f) {
		return cfg_.zoom_max;
	}
	return hit;
}

void MeridianTpsCamera::apply_transforms() {
	// This node's yaw is the world camera orbit angle (its parent is the
	// un-rotated position anchor — see header).
	set_rotation(Vector3(0.0f, core_.camera_yaw(), 0.0f));
	if (pivot_ != nullptr) {
		// Negate so a MORE-positive core pitch raises the camera toward top-down.
		pivot_->set_rotation(Vector3(-core_.pitch(), 0.0f, 0.0f));
	}
	if (camera_ != nullptr) {
		camera_->set_position(Vector3(0.0f, 0.0f, core_.boom_length()));
	}
	if (yaw_target_ != nullptr) {
		yaw_target_->set_rotation(Vector3(0.0f, core_.character_yaw(), 0.0f));
	}
}

void MeridianTpsCamera::step(double delta) {
	core_.advance(static_cast<float>(delta), query_collision_distance());
	apply_transforms();
}

// ===========================================================================
// Test / headless drivers
// ===========================================================================
void MeridianTpsCamera::feed_mouse_motion(float dx, float dy, int mode) {
	camera::DragMode m = camera::DragMode::None;
	if (mode == 1) m = camera::DragMode::Steer;
	else if (mode == 2) m = camera::DragMode::Orbit;
	core_.on_mouse_motion(dx, dy, m);
}

void MeridianTpsCamera::feed_zoom(int notches) {
	core_.on_zoom(notches);
}

Dictionary MeridianTpsCamera::get_state() const {
	Dictionary d;
	d["camera_yaw"]    = core_.camera_yaw();
	d["character_yaw"] = core_.character_yaw();
	d["pitch"]         = core_.pitch();
	d["zoom_target"]   = core_.zoom_target();
	d["zoom_length"]   = core_.zoom_length();
	d["boom_length"]   = core_.boom_length();
	d["hit_length"]    = query_collision_distance();
	d["camera_global_position"] =
	    (camera_ != nullptr) ? camera_->get_global_position() : Vector3();
	return d;
}

// ===========================================================================
// Property accessors
// ===========================================================================
void MeridianTpsCamera::set_yaw_sensitivity(float v) { cfg_.yaw_sensitivity = v; }
float MeridianTpsCamera::get_yaw_sensitivity() const { return cfg_.yaw_sensitivity; }
void MeridianTpsCamera::set_pitch_sensitivity(float v) { cfg_.pitch_sensitivity = v; }
float MeridianTpsCamera::get_pitch_sensitivity() const { return cfg_.pitch_sensitivity; }
void MeridianTpsCamera::set_pitch_min(float v) { cfg_.pitch_min = v; }
float MeridianTpsCamera::get_pitch_min() const { return cfg_.pitch_min; }
void MeridianTpsCamera::set_pitch_max(float v) { cfg_.pitch_max = v; }
float MeridianTpsCamera::get_pitch_max() const { return cfg_.pitch_max; }
void MeridianTpsCamera::set_zoom_min(float v) { cfg_.zoom_min = v; }
float MeridianTpsCamera::get_zoom_min() const { return cfg_.zoom_min; }
void MeridianTpsCamera::set_zoom_max(float v) { cfg_.zoom_max = v; }
float MeridianTpsCamera::get_zoom_max() const { return cfg_.zoom_max; }
void MeridianTpsCamera::set_zoom_step(float v) { cfg_.zoom_step = v; }
float MeridianTpsCamera::get_zoom_step() const { return cfg_.zoom_step; }
void MeridianTpsCamera::set_zoom_default(float v) { cfg_.zoom_default = v; }
float MeridianTpsCamera::get_zoom_default() const { return cfg_.zoom_default; }
void MeridianTpsCamera::set_zoom_spring(float v) { cfg_.zoom_spring = v; }
float MeridianTpsCamera::get_zoom_spring() const { return cfg_.zoom_spring; }
void MeridianTpsCamera::set_boom_spring(float v) { cfg_.boom_spring = v; }
float MeridianTpsCamera::get_boom_spring() const { return cfg_.boom_spring; }

void MeridianTpsCamera::set_camera_height(float v) { camera_height_ = v; }
float MeridianTpsCamera::get_camera_height() const { return camera_height_; }
void MeridianTpsCamera::set_probe_radius(float v) { probe_radius_ = v; }
float MeridianTpsCamera::get_probe_radius() const { return probe_radius_; }
void MeridianTpsCamera::set_collision_mask(uint32_t v) {
	collision_mask_ = v;
	if (probe_ != nullptr) probe_->set_collision_mask(v);
}
uint32_t MeridianTpsCamera::get_collision_mask() const { return collision_mask_; }
void MeridianTpsCamera::set_capture_mouse(bool v) { capture_mouse_ = v; }
bool MeridianTpsCamera::get_capture_mouse() const { return capture_mouse_; }
void MeridianTpsCamera::set_yaw_target_path(const NodePath &p) {
	yaw_target_path_ = p;
	if (is_inside_tree()) {
		yaw_target_ = Object::cast_to<Node3D>(get_node_or_null(p));
	}
}
NodePath MeridianTpsCamera::get_yaw_target_path() const { return yaw_target_path_; }

// --- resolved state ---
float MeridianTpsCamera::get_boom_length() const { return core_.boom_length(); }
float MeridianTpsCamera::get_camera_yaw() const { return core_.camera_yaw(); }
float MeridianTpsCamera::get_character_yaw() const { return core_.character_yaw(); }
float MeridianTpsCamera::get_pitch() const { return core_.pitch(); }
float MeridianTpsCamera::get_zoom() const { return core_.zoom_target(); }

}  // namespace meridian
