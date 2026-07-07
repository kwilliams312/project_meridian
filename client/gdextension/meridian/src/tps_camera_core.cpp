// Project Meridian — third-person WoW camera CORE implementation (issue #105).
// Engine-free (no Godot); see tps_camera_core.h for the design rationale.

#include "tps_camera_core.h"

#include <cmath>

namespace meridian::camera {

namespace {
constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
}  // namespace

float clampf(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

float wrap_angle(float a) {
	// Standard floor reduction to [-pi, pi) — numerically robust (no accumulation
	// of fmod's dividend-sign quirks at the boundary) — then fold the -pi endpoint
	// (and any floating-point dust just above it) up to +pi so the range is the
	// half-open (-pi, pi]. "Facing backward" thus reads as +pi, not -pi.
	a -= kTwoPi * std::floor((a + kPi) / kTwoPi);
	if (a <= -kPi + 1e-4f) {
		a = kPi;
	}
	return a;
}

TpsCamera::TpsCamera() : TpsCamera(CameraConfig{}) {}

TpsCamera::TpsCamera(const CameraConfig &cfg) : cfg_(cfg) {
	reset(0.0f, 0.0f, cfg_.zoom_default);
}

void TpsCamera::reset(float character_yaw, float pitch, float zoom) {
	character_yaw_ = wrap_angle(character_yaw);
	camera_yaw_    = character_yaw_;  // camera starts locked behind the character
	pitch_         = clampf(pitch, cfg_.pitch_min, cfg_.pitch_max);
	zoom_target_   = clampf(zoom, cfg_.zoom_min, cfg_.zoom_max);
	zoom_len_      = zoom_target_;  // no spring on an explicit reset
	boom_len_      = zoom_target_;
}

void TpsCamera::on_mouse_motion(float dx, float dy, DragMode mode) {
	if (mode == DragMode::None) {
		return;
	}
	// Horizontal: turning right (dx > 0) yaws the view clockwise, which is a
	// DECREASE in Godot's +Y-is-counter-clockwise convention — hence the minus.
	const float dyaw = -dx * cfg_.yaw_sensitivity;
	camera_yaw_ = wrap_angle(camera_yaw_ + dyaw);
	if (mode == DragMode::Steer) {
		// Steer turns the character too, so the camera stays locked behind it.
		character_yaw_ = wrap_angle(character_yaw_ + dyaw);
	}
	// Vertical: same for both modes — pitch the camera, clamped off the ground
	// and away from the straight-overhead gimbal.
	pitch_ = clampf(pitch_ + dy * cfg_.pitch_sensitivity, cfg_.pitch_min, cfg_.pitch_max);
}

void TpsCamera::on_zoom(int notches) {
	zoom_target_ = clampf(zoom_target_ + static_cast<float>(notches) * cfg_.zoom_step,
	                      cfg_.zoom_min, cfg_.zoom_max);
}

float TpsCamera::advance(float dt, float collision_distance) {
	if (dt < 0.0f) {
		dt = 0.0f;
	}
	// 1. Ease the wheel-zoom length toward the (clamped) target, both directions,
	//    so a scroll notch glides instead of snapping.
	const float dz = cfg_.zoom_spring * dt;
	if (zoom_len_ < zoom_target_) {
		zoom_len_ = std::fmin(zoom_target_, zoom_len_ + dz);
	} else {
		zoom_len_ = std::fmax(zoom_target_, zoom_len_ - dz);
	}

	// 2. Cap the boom by the nearest obstruction (never longer than the eased
	//    zoom length, never negative).
	const float clear = collision_distance < 0.0f ? 0.0f : collision_distance;
	const float limit = std::fmin(zoom_len_, clear);

	// 3. Instant pull-IN when the cap tightens (never clip through geometry);
	//    smooth spring-OUT (boom_spring m/s) when it relaxes.
	if (limit < boom_len_) {
		boom_len_ = limit;
	} else {
		boom_len_ = std::fmin(limit, boom_len_ + cfg_.boom_spring * dt);
	}
	return boom_len_;
}

}  // namespace meridian::camera
