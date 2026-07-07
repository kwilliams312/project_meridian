// Project Meridian — third-person "WoW-style" camera CORE (issue #105).
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores"): this header
// and its .cpp contain NO Godot types. The GDExtension node
// (`MeridianTpsCamera`, meridian_tps_camera.*) is a thin wrapper that reads
// Godot input, feeds it to this core, and applies the resolved yaw / pitch /
// boom-length back onto a SpringArm3D + Camera3D rig. Keeping the input→state
// math engine-free means the whole camera state machine (mode selection, angle
// accumulation, clamps, zoom + collision-boom springs) is unit-tested in a plain
// C++17 doctest with no Godot runtime — the same discipline as the #102 movement
// controller and #104 remote interpolator cores.
//
// The behaviour this implements (Client SAD §363 / Client PRD §134 CHR-02:
// "third-person camera with WoW-style mouse controls"):
//   * Hold-RMB "steer"  — mouse-X turns the CHARACTER and the camera together,
//                         so the camera stays locked directly behind the player;
//                         mouse-Y pitches the camera.
//   * Hold-LMB "orbit"  — mouse-X/Y orbit the camera AROUND the character; the
//                         character's facing is unchanged.
//   * Mouse wheel       — zooms the boom in/out, clamped to [zoom_min, zoom_max].
//   * Collision boom    — the arm shortens instantly when geometry is between the
//                         camera and the character, and springs back out smoothly
//                         once the line of sight is clear.
//
// The engine-free core owns all of that math. The Godot node supplies only the
// per-frame collision distance (from a SpringArm3D probe or a raycast) and turns
// the resolved scalars into transforms.

#ifndef MERIDIAN_TPS_CAMERA_CORE_H
#define MERIDIAN_TPS_CAMERA_CORE_H

namespace meridian::camera {

// ===========================================================================
// Tuning constants — sensible WoW-like DEFAULTS (issue #105).
//
// The Client SAD/PRD call for "WoW-style mouse controls" but do not pin numeric
// clamps, so these are documented defaults chosen to feel like classic WoW's
// third-person camera. Every one is overridable via CameraConfig (exposed on the
// GDExtension node as editor properties) — there are deliberately NO magic
// numbers scattered through the logic.
// ===========================================================================
namespace defaults {

// Mouse sensitivity, radians of rotation per pixel of mouse motion. ~0.4°/px.
constexpr float kYawSensitivity   = 0.007f;
constexpr float kPitchSensitivity = 0.007f;

// Pitch (camera elevation) clamp, radians. Measured as rotation about the rig's
// local X: more-positive pitch raises the camera and tilts it DOWN toward the
// character (approaching top-down); more-negative lowers it to look UP. The
// limits keep the camera off the ground and away from the straight-overhead
// gimbal singularity.
//   kPitchMin ≈ -20° (look slightly up from below the character)
//   kPitchMax ≈ +75° (steep, near-overhead, but not vertical)
constexpr float kPitchMin = -0.35f;  // rad  (~ -20°)
constexpr float kPitchMax = 1.30f;   // rad  (~ +74.5°)

// Zoom (boom length) clamp + wheel step, metres. WoW-like pullback range.
constexpr float kZoomMin  = 1.5f;    // closest 3rd-person pull-in
constexpr float kZoomMax  = 20.0f;   // farthest pullback
constexpr float kZoomStep = 1.5f;    // per wheel notch
constexpr float kZoomDefault = 6.0f; // starting boom length

// Spring rates, metres per second of ease-out.
//   kZoomSpring — how fast the boom eases toward a new wheel-zoom target (both
//                 directions), so a scroll notch glides instead of snapping.
//   kBoomSpring — how fast the boom springs BACK OUT after a collision clears.
//                 Pull-IN on collision is instantaneous (never clip through
//                 geometry); only the recovery is smoothed.
constexpr float kZoomSpring = 12.0f;
constexpr float kBoomSpring = 8.0f;

}  // namespace defaults

// Which mouse button (if any) is currently held while the mouse moves. Selects
// how a mouse-motion sample is interpreted (steer vs orbit). If BOTH buttons are
// held, WoW treats it as steer (RMB wins) — see MeridianTpsCamera.
enum class DragMode : int {
	None  = 0,  // no button held — motion ignored
	Steer = 1,  // RMB — turn character + camera together (camera locked behind)
	Orbit = 2,  // LMB — orbit camera around character; character facing unchanged
};

// All tunables in one struct so the node can expose them as editor properties
// and tests can construct deterministic configurations. Defaults are the
// documented WoW-like values above.
struct CameraConfig {
	float yaw_sensitivity   = defaults::kYawSensitivity;
	float pitch_sensitivity = defaults::kPitchSensitivity;
	float pitch_min         = defaults::kPitchMin;
	float pitch_max         = defaults::kPitchMax;
	float zoom_min          = defaults::kZoomMin;
	float zoom_max          = defaults::kZoomMax;
	float zoom_step         = defaults::kZoomStep;
	float zoom_default      = defaults::kZoomDefault;
	float zoom_spring       = defaults::kZoomSpring;
	float boom_spring       = defaults::kBoomSpring;
};

// The pure WoW-camera state machine. Deterministic function of its inputs — no
// clocks, no I/O, no Godot. `advance(dt, collision_distance)` is the only method
// that consults time; everything else just mutates angle/zoom accumulators.
class TpsCamera {
public:
	TpsCamera();
	explicit TpsCamera(const CameraConfig &cfg);

	// Seed the camera to a known orientation + zoom (e.g. on spawn). `zoom` is
	// clamped to [zoom_min, zoom_max]; `pitch` is clamped to [pitch_min,
	// pitch_max]. The boom snaps to `zoom` (no spring on reset).
	void reset(float character_yaw, float pitch, float zoom);

	// Feed one mouse-motion sample (pixels). `dx` is +right, `dy` is +down (Godot
	// screen convention). Behaviour depends on `mode`:
	//   Steer — character_yaw and camera_yaw both turn by -dx*yaw_sensitivity
	//           (camera stays behind the character); pitch += dy*pitch_sensitivity.
	//   Orbit — only camera_yaw turns by -dx*yaw_sensitivity (character unchanged);
	//           pitch += dy*pitch_sensitivity.
	//   None  — ignored.
	// Pitch is always clamped; yaws are wrapped to (-pi, pi].
	void on_mouse_motion(float dx, float dy, DragMode mode);

	// Apply `notches` wheel steps to the zoom target. Positive = zoom OUT (boom
	// longer), negative = zoom IN (boom shorter) — the node maps WHEEL_DOWN→+1,
	// WHEEL_UP→-1 to match WoW. The target is clamped to [zoom_min, zoom_max]; the
	// boom eases toward it over subsequent advance() calls.
	void on_zoom(int notches);

	// Advance the two boom springs by `dt` seconds and return the resolved boom
	// length to place the camera at.
	//   `collision_distance` — the maximum UNOBSTRUCTED boom length this frame
	//       (distance from the pivot to the first obstruction along the boom).
	//       Pass >= zoom_max (or the SpringArm's hit length) when the view is
	//       clear. Negative values are treated as 0.
	// Rule: the boom follows the eased zoom length, but is capped by
	// `collision_distance`; it snaps IN immediately when the cap tightens and
	// springs OUT (boom_spring m/s) when the cap relaxes.
	float advance(float dt, float collision_distance);

	// --- Resolved state (read by the node each frame, and by tests) ---
	float character_yaw() const { return character_yaw_; }
	float camera_yaw() const { return camera_yaw_; }
	float pitch() const { return pitch_; }
	float zoom_target() const { return zoom_target_; }  // clamped wheel target
	float zoom_length() const { return zoom_len_; }     // eased toward target
	float boom_length() const { return boom_len_; }     // collision-resolved
	const CameraConfig &config() const { return cfg_; }

private:
	CameraConfig cfg_;
	float character_yaw_ = 0.0f;  // player facing (rad), driven only in Steer
	float camera_yaw_    = 0.0f;  // camera orbit angle (rad)
	float pitch_         = 0.0f;  // camera elevation (rad), clamped
	float zoom_target_   = 0.0f;  // wheel target boom length (metres)
	float zoom_len_      = 0.0f;  // eased toward zoom_target_
	float boom_len_      = 0.0f;  // collision-resolved actual camera distance
};

// Wrap an angle to (-pi, pi]. Exposed for tests.
float wrap_angle(float radians);

// Clamp helper (engine-free; std::clamp is C++17 but kept local for clarity).
float clampf(float v, float lo, float hi);

}  // namespace meridian::camera

#endif  // MERIDIAN_TPS_CAMERA_CORE_H
