# render_measure.gd — windowed render + frame-time capture (issue #132).
#
# Renders a one-chunk Terrain3D (built through the ITerrainBackend seam) for a
# fixed number of frames, records the rolling frame time, writes a screenshot,
# and quits. Needs a GPU/display — run WITHOUT --headless:
#   godot --path <spike> --script res://spike/render_measure.gd --resolution 1920x1080
#
# IMPORTANT: any number this produces on a dev box is NOT the min-spec figure.
# The C2 gate (docs/terrain-eval.md) is 30 FPS @ 1080p Low on a GTX 1060 6 GB
# (#61 / TD-03). This script is the exact rig to run on that bench; on any other
# machine the number is only a smoke-test that the terrain renders + moves LODs.
extends SceneTree

const Terrain3DBackendC := preload("res://seam/terrain3d_backend.gd")
const WARMUP_FRAMES := 60
const MEASURE_FRAMES := 240

var _root3d: Node3D
var _cam: Camera3D
var _backend: ITerrainBackend
var _frame := 0
var _accum_ms := 0.0
var _last_tick_us := 0
var _worst_ms := 0.0
var _shot_path := "user://terrain3d_spike_render.png"

func _init() -> void:
	process_frame.connect(_setup, ConnectFlags.CONNECT_ONE_SHOT)

func _setup() -> void:
	# Uncap the frame rate so the measured number reflects GPU cost, not the
	# display refresh (otherwise this reads as ~vsync FPS and misleads).
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0

	_root3d = Node3D.new()
	get_root().add_child(_root3d)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45, -30, 0)
	_root3d.add_child(sun)

	_cam = Camera3D.new()
	_cam.position = Vector3(64, 80, 210)
	_cam.look_at(Vector3(64, 0, 64), Vector3.UP)
	_cam.current = true
	_root3d.add_child(_cam)

	if not ClassDB.class_exists("Terrain3D"):
		push_error("Terrain3D extension not loaded — build it first.")
		quit(2)
		return

	var terrain: Node = ClassDB.instantiate("Terrain3D")
	_root3d.add_child(terrain)
	if ClassDB.class_exists("Terrain3DMaterial"):
		terrain.set("material", ClassDB.instantiate("Terrain3DMaterial"))
	# Terrain3D needs a clipmap target/camera to update LODs (the whole point of
	# the C2 measurement) — wire the camera through its documented API.
	if terrain.has_method("set_camera"):
		terrain.set_camera(_cam)

	_backend = Terrain3DBackendC.new(terrain)
	_backend.sculpt({"center": Vector3(64, 0, 64), "radius": 55.0, "strength": 30.0})
	print("Render-measure: terrain built via %s; warming up..." % _backend.backend_name())

	process_frame.connect(_on_frame)

func _on_frame() -> void:
	_frame += 1
	# Orbit the camera so the geoclipmap LODs actually update each frame.
	var a := _frame * 0.01
	_cam.position = Vector3(64 + cos(a) * 150.0, 80, 64 + sin(a) * 150.0)
	_cam.look_at(Vector3(64, 0, 64), Vector3.UP)

	# Real per-frame wall time (ticks_usec), not the 1 s FPS average.
	var now := Time.get_ticks_usec()
	if _frame > WARMUP_FRAMES and _last_tick_us > 0:
		var ms := (now - _last_tick_us) / 1000.0
		_accum_ms += ms
		_worst_ms = maxf(_worst_ms, ms)
	_last_tick_us = now

	if _frame == WARMUP_FRAMES + 1:
		var img := get_root().get_texture().get_image()
		if img != null:
			img.save_png(_shot_path)
			print("Screenshot saved: %s" % ProjectSettings.globalize_path(_shot_path))

	if _frame >= WARMUP_FRAMES + MEASURE_FRAMES:
		var avg := _accum_ms / (MEASURE_FRAMES - 1)
		var vp := get_root().get_visible_rect().size
		print("\n=== RENDER MEASURE (THIS BOX — NOT MIN-SPEC) ===")
		print("  gpu / renderer : %s (Forward+)" % RenderingServer.get_video_adapter_name())
		print("  resolution     : %dx%d" % [vp.x, vp.y])
		print("  frames         : %d measured (%d warmup), vsync off" % [MEASURE_FRAMES, WARMUP_FRAMES])
		print("  avg frame      : %.3f ms  (~%.0f FPS)" % [avg, 1000.0 / avg])
		print("  worst frame    : %.3f ms" % _worst_ms)
		print("  NOTE: min-spec C2 (30 FPS @ 1080p Low, GTX 1060 6 GB, #61/TD-03)")
		print("        must be run on the bench with this same script.")
		quit(0)
