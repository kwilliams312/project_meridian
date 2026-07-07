// Project Meridian — engine-free unit test for the third-person WoW camera core
// (issue #105). NO Godot: it compiles against the plain-C++ core
// (tps_camera_core.*) only, so it runs in any C++17 toolchain without a Godot
// runtime (Client SAD §9.2 engine-agnostic cores). Plain-main style, mirroring
// the #102 movement_controller_test / #104 remote_interpolation_test, ctest-wired
// via client/gdextension/meridian/test/CMakeLists.txt.
//
// Proves the #105 input→state math the node relies on:
//   1. STEER  (RMB) — mouse-X turns BOTH character + camera (locked behind);
//                     mouse-Y pitches, clamped.
//   2. ORBIT  (LMB) — mouse-X/Y orbit the CAMERA only; character facing frozen.
//   3. ZOOM         — wheel target clamps to [zoom_min, zoom_max]; boom eases in.
//   4. PITCH CLAMP  — pitch never exceeds [pitch_min, pitch_max].
//   5. COLLISION BOOM — snaps IN instantly on obstruction, springs OUT smoothly
//                       when clear, never overshoots the eased zoom length.
//   6. WRAP         — yaw wraps to (-pi, pi].

#include "tps_camera_core.h"

#include <cmath>
#include <cstdio>

namespace cam = meridian::camera;

static int g_fail = 0;
static void check(const char *name, bool ok) {
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++g_fail;
}

static bool near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps;
}

static constexpr float kPi = 3.14159265358979323846f;

int main() {
	std::printf("meridian tps camera core test (#105)\n");
	const cam::CameraConfig cfg;  // documented WoW-like defaults

	// =======================================================================
	// 1. STEER (RMB): mouse-X turns character AND camera together; the camera
	//    stays locked behind (camera_yaw == character_yaw).
	// =======================================================================
	std::printf("[1] steer: character + camera turn together\n");
	{
		cam::TpsCamera c(cfg);
		c.reset(0.0f, 0.0f, cfg.zoom_default);
		const float dx = 50.0f;
		c.on_mouse_motion(dx, 0.0f, cam::DragMode::Steer);
		const float expect = cam::wrap_angle(-dx * cfg.yaw_sensitivity);
		check("character_yaw turned by -dx*sens", near(c.character_yaw(), expect));
		check("camera_yaw equals character_yaw (locked behind)",
		      near(c.camera_yaw(), c.character_yaw()));
	}

	// =======================================================================
	// 2. ORBIT (LMB): mouse-X orbits the CAMERA only; character facing frozen.
	// =======================================================================
	std::printf("[2] orbit: camera orbits, character facing frozen\n");
	{
		cam::TpsCamera c(cfg);
		c.reset(0.0f, 0.0f, cfg.zoom_default);
		const float dx = 40.0f;
		c.on_mouse_motion(dx, 0.0f, cam::DragMode::Orbit);
		check("character_yaw unchanged", near(c.character_yaw(), 0.0f));
		check("camera_yaw orbited by -dx*sens",
		      near(c.camera_yaw(), cam::wrap_angle(-dx * cfg.yaw_sensitivity)));
		// A subsequent steer must re-align the camera behind the (turned) char.
		const float cam_before = c.camera_yaw();
		c.on_mouse_motion(10.0f, 0.0f, cam::DragMode::Steer);
		check("steer still offsets camera by the same delta as character",
		      near(c.camera_yaw() - cam_before,
		           c.character_yaw() - 0.0f));  // both moved by the steer delta
	}

	// =======================================================================
	// 3. ZOOM: wheel clamps target to [zoom_min, zoom_max]; boom eases toward it.
	// =======================================================================
	std::printf("[3] zoom: wheel clamps + boom eases toward target\n");
	{
		cam::TpsCamera c(cfg);
		c.reset(0.0f, 0.0f, cfg.zoom_default);
		// Zoom way out — target must clamp at zoom_max, not exceed it.
		c.on_zoom(+100);
		check("zoom target clamps at zoom_max", near(c.zoom_target(), cfg.zoom_max));
		// Zoom way in — target clamps at zoom_min.
		c.on_zoom(-1000);
		check("zoom target clamps at zoom_min", near(c.zoom_target(), cfg.zoom_min));
		// One notch out from min, then ease: after enough advance the eased zoom
		// length converges on the target and the (unobstructed) boom follows.
		c.on_zoom(+1);
		const float tgt = c.zoom_target();
		for (int i = 0; i < 240; ++i) c.advance(1.0f / 60.0f, cfg.zoom_max);
		check("eased zoom length converges to target", near(c.zoom_length(), tgt, 1e-2f));
		check("unobstructed boom equals eased zoom length",
		      near(c.boom_length(), c.zoom_length(), 1e-2f));
	}

	// =======================================================================
	// 4. PITCH CLAMP: pitch saturates at the configured limits.
	// =======================================================================
	std::printf("[4] pitch clamp: saturates at [pitch_min, pitch_max]\n");
	{
		cam::TpsCamera c(cfg);
		c.reset(0.0f, 0.0f, cfg.zoom_default);
		// Push pitch hard positive (mouse down) far past the max.
		c.on_mouse_motion(0.0f, 100000.0f, cam::DragMode::Orbit);
		check("pitch clamps at pitch_max", near(c.pitch(), cfg.pitch_max));
		// Push hard negative (mouse up).
		c.on_mouse_motion(0.0f, -1000000.0f, cam::DragMode::Orbit);
		check("pitch clamps at pitch_min", near(c.pitch(), cfg.pitch_min));
	}

	// =======================================================================
	// 5. COLLISION BOOM: snaps IN instantly, springs OUT smoothly, never past
	//    the eased zoom length.
	// =======================================================================
	std::printf("[5] collision boom: instant pull-in, smooth spring-out\n");
	{
		cam::TpsCamera c(cfg);
		c.reset(0.0f, 0.0f, cfg.zoom_default);
		// Settle unobstructed at the default zoom.
		for (int i = 0; i < 300; ++i) c.advance(1.0f / 60.0f, cfg.zoom_max);
		check("settled boom at default zoom", near(c.boom_length(), cfg.zoom_default, 1e-2f));

		// Obstruction appears 2 m away: boom must snap IN this very frame.
		const float obstruction = 2.0f;
		const float len = c.advance(1.0f / 60.0f, obstruction);
		check("boom snaps IN to obstruction in one frame", near(len, obstruction, 1e-3f));

		// Obstruction clears: boom must NOT jump back — it springs out gradually.
		const float one = c.advance(1.0f / 60.0f, cfg.zoom_max);
		check("boom does NOT snap back out (springs)", one < cfg.zoom_default - 0.5f);
		check("boom moved out by ~boom_spring*dt",
		      near(one - obstruction, cfg.boom_spring * (1.0f / 60.0f), 1e-2f));

		// Given time, it springs fully back out to the eased zoom length.
		for (int i = 0; i < 600; ++i) c.advance(1.0f / 60.0f, cfg.zoom_max);
		check("boom springs back out to zoom length", near(c.boom_length(), c.zoom_length(), 1e-2f));
		check("boom never exceeds eased zoom length", c.boom_length() <= c.zoom_length() + 1e-3f);
	}

	// =======================================================================
	// 6. WRAP: yaw stays wrapped to (-pi, pi] under large accumulation.
	// =======================================================================
	std::printf("[6] wrap: yaw stays within (-pi, pi]\n");
	{
		cam::TpsCamera c(cfg);
		c.reset(0.0f, 0.0f, cfg.zoom_default);
		for (int i = 0; i < 100; ++i)
			c.on_mouse_motion(500.0f, 0.0f, cam::DragMode::Orbit);
		check("camera_yaw wrapped in (-pi, pi]",
		      c.camera_yaw() > -kPi - 1e-4f && c.camera_yaw() <= kPi + 1e-4f);
		check("wrap_angle(3pi) == pi", near(cam::wrap_angle(3.0f * kPi), kPi, 1e-4f));
		check("wrap_angle(-3pi) == pi", near(cam::wrap_angle(-3.0f * kPi), kPi, 1e-4f));
	}

	std::printf(g_fail == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
