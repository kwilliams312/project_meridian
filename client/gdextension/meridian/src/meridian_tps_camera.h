// Project Meridian — third-person WoW camera GDExtension node (issue #105).
//
// Thin Godot glue over the engine-free core (tps_camera_core.*). The core owns
// ALL the input→state math (steer-vs-orbit mode, yaw/pitch accumulation +
// clamps, wheel-zoom clamping, and the collision-boom springs); this node only:
//   * builds the rig at runtime — a PitchPivot (Node3D) holding a SpringArm3D
//     collision probe and a Camera3D, so instantiating the class is enough (no
//     hand-authored .tscn required),
//   * reads Godot mouse input (hold-RMB steer / hold-LMB orbit / wheel zoom) and
//     feeds it to the core,
//   * each physics frame queries the SpringArm3D for the nearest obstruction and
//     hands that distance to the core, then applies the resolved yaw / pitch /
//     boom-length back onto the rig transforms.
//
// Why a C++ GDExtension node (not GDScript): the whole client sim surface —
// movement (#102), remote interpolation (#104) — is C++ GDExtension with an
// engine-free, unit-tested core (Client SAD §9.2). GDScript in this project is
// reserved for UI scene glue (login_flow.gd, login_screen.gd). A per-frame
// input→transform camera with a testable spring core fits the C++ sim pattern
// exactly, so it lives here alongside MeridianMovementController. Unlike those
// RefCounted cores, this class extends Node3D because a camera RIG must live in
// the scene tree (it owns a Camera3D + SpringArm3D and follows the player).
//
// Recommended scene setup (see client/project/scenes/world/camera_demo.tscn):
//   Player (Node3D)                 <- position anchor (NOT yaw-rotated)
//   ├── Body  (MeshInstance3D)      <- yaw_target: this node rotates it to face
//   └── MeridianTpsCamera           <- rotation.y = camera_yaw (world)
//        └── PitchPivot / SpringArm3D + Camera3D  (built in _ready)
// Keeping the camera's parent un-rotated means camera_yaw is a clean world angle
// and the character's facing lives on `yaw_target`, so steer and orbit never
// double-count each other's rotation.

#ifndef MERIDIAN_TPS_CAMERA_NODE_H
#define MERIDIAN_TPS_CAMERA_NODE_H

#include "tps_camera_core.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event.hpp>  // complete type: _unhandled_input virtual
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/spring_arm3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>

namespace meridian {

class MeridianTpsCamera : public godot::Node3D {
	GDCLASS(MeridianTpsCamera, godot::Node3D)

protected:
	static void _bind_methods();

public:
	MeridianTpsCamera();
	~MeridianTpsCamera();

	// --- Godot lifecycle (auto-called; not bound) ---
	void _ready() override;
	void _physics_process(double delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &event) override;

	// --- Config properties (exposed to the editor + GDScript) ---
	void set_yaw_sensitivity(float v);
	float get_yaw_sensitivity() const;
	void set_pitch_sensitivity(float v);
	float get_pitch_sensitivity() const;
	void set_pitch_min(float v);
	float get_pitch_min() const;
	void set_pitch_max(float v);
	float get_pitch_max() const;
	void set_zoom_min(float v);
	float get_zoom_min() const;
	void set_zoom_max(float v);
	float get_zoom_max() const;
	void set_zoom_step(float v);
	float get_zoom_step() const;
	void set_zoom_default(float v);
	float get_zoom_default() const;
	void set_zoom_spring(float v);
	float get_zoom_spring() const;
	void set_boom_spring(float v);
	float get_boom_spring() const;

	void set_camera_height(float v);
	float get_camera_height() const;
	void set_probe_radius(float v);
	float get_probe_radius() const;
	void set_collision_mask(uint32_t v);
	uint32_t get_collision_mask() const;
	void set_capture_mouse(bool v);
	bool get_capture_mouse() const;
	void set_yaw_target_path(const godot::NodePath &p);
	godot::NodePath get_yaw_target_path() const;

	// --- Resolved state accessors ---
	float get_boom_length() const;
	float get_camera_yaw() const;
	float get_character_yaw() const;
	float get_pitch() const;
	float get_zoom() const;

	// --- Headless / test drivers (bound) ---
	// These let a --headless GDScript exercise the exact runtime path without a
	// real mouse: feed input, step the boom, and read the whole state at once.
	void feed_mouse_motion(float dx, float dy, int mode);  // mode: 0 none,1 steer,2 orbit
	void feed_zoom(int notches);
	void step(double delta);          // advance boom + apply transforms once
	godot::Dictionary get_state() const;

private:
	camera::CameraConfig cfg_;
	camera::TpsCamera core_;

	// Rig nodes, built in _ready().
	godot::Node3D *pivot_       = nullptr;  // pitch pivot at head height
	godot::SpringArm3D *probe_  = nullptr;  // collision probe (no children)
	godot::Camera3D *camera_    = nullptr;  // the actual view camera
	godot::Node3D *yaw_target_  = nullptr;  // resolved from yaw_target_path

	// Config not carried by CameraConfig.
	float camera_height_    = 1.6f;   // pivot height above the player origin (m)
	float probe_radius_     = 0.25f;  // SpringArm sphere probe radius (m)
	uint32_t collision_mask_ = 1;     // physics layers the boom collides with
	bool capture_mouse_     = true;   // hide + capture cursor while dragging
	godot::NodePath yaw_target_path_;

	// Live drag state (from mouse buttons).
	bool rmb_held_ = false;  // steer
	bool lmb_held_ = false;  // orbit

	void build_rig();
	void apply_transforms();
	camera::DragMode current_drag_mode() const;
	float query_collision_distance() const;
	void update_mouse_capture();
};

}  // namespace meridian

#endif  // MERIDIAN_TPS_CAMERA_NODE_H
