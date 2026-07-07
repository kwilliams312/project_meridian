# spike_main.gd — the WINDOWED render scene (issue #132).
#
# Builds a one-chunk Terrain3D through the ITerrainBackend seam, sculpts a hill,
# frames a camera on it, and shows a live FPS / frame-time overlay. Two jobs:
#   1. Visual confirmation the terrain RENDERS (the owner eyeballs this — flagged
#      in the report as owner-confirm, since a headless box can't see pixels).
#   2. The frame-time capture rig for the min-spec bench (#61 / TD-03). On the
#      GTX 1060 6 GB bench at 1080p Low, read the overlay's rolling avg frame ms;
#      that is the C2 number docs/terrain-eval.md leaves open.
#
# Everything terrain goes through the seam — no direct Terrain3D calls here.
extends Node3D

const Terrain3DBackendC := preload("res://seam/terrain3d_backend.gd")

var _backend: ITerrainBackend
var _label: Label
var _frame_ms_accum := 0.0
var _frames := 0
var _rolling_ms := 0.0

func _ready() -> void:
	# Light + camera so the terrain is actually lit and framed.
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45, -30, 0)
	add_child(sun)

	var cam := Camera3D.new()
	cam.position = Vector3(64, 70, 200)
	cam.look_at(Vector3(64, 0, 64), Vector3.UP)
	cam.current = true
	add_child(cam)

	if not ClassDB.class_exists("Terrain3D"):
		_make_overlay("Terrain3D extension NOT loaded — build it (build_terrain3d.sh)")
		push_error("Terrain3D GDExtension not present.")
		return

	var terrain: Node = ClassDB.instantiate("Terrain3D")
	add_child(terrain)
	# A default material so the clipmap mesh has something to draw.
	if terrain.has_method("set_material") and ClassDB.class_exists("Terrain3DMaterial"):
		terrain.set("material", ClassDB.instantiate("Terrain3DMaterial"))

	_backend = Terrain3DBackendC.new(terrain)
	# Sculpt a hill through the seam (op 1).
	_backend.sculpt({"center": Vector3(64, 0, 64), "radius": 50.0, "strength": 25.0})

	_make_overlay("Terrain3D spike — rendering. FPS overlay below.")

func _make_overlay(msg: String) -> void:
	var canvas := CanvasLayer.new()
	add_child(canvas)
	_label = Label.new()
	_label.position = Vector2(16, 16)
	_label.text = msg
	canvas.add_child(_label)

func _process(delta: float) -> void:
	_frame_ms_accum += delta * 1000.0
	_frames += 1
	if _frames >= 30:
		_rolling_ms = _frame_ms_accum / _frames
		_frame_ms_accum = 0.0
		_frames = 0
	if _label:
		_label.text = "Terrain3D spike (#132) — behind ITerrainBackend seam\n" + \
			"FPS: %d   frame: %.2f ms (rolling avg)\n" % [Engine.get_frames_per_second(), _rolling_ms] + \
			"Backend: %s\n" % (_backend.backend_name() if _backend else "none") + \
			"For the C2 min-spec number: run this on the GTX 1060 bench @ 1080p Low."
