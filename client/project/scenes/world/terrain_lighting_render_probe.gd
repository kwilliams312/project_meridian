# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — A/B RENDER PROBE for the chibi meadow's lighting (#887, epic
# #872 T5). NOT a shipped scene, and NOT a pass/fail gate: it renders the REAL staged
# Sprout Meadow chunks under the M0 lighting and under #887's, and writes both PNGs so
# a human can look at the two and judge whether the hills read as hills.
#
# It exists because this story is VISUAL and the failure mode here is trusting a green
# check. world_lighting_verify.gd asserts a Lambert MODEL of the shading — it can be
# perfectly green while the screen still reads flat. #875's winding bug passed every
# test while rendering nothing at all. The only honest evidence is the picture.
#
# Must run WINDOWED (a real GPU device) — `--headless` draws nothing, and on macOS the
# headless path aborts inside MoltenVK anyway (#283):
#
#   /Applications/Godot.app/Contents/MacOS/Godot --rendering-driver metal \
#     --path client/project --script res://scenes/world/terrain_lighting_render_probe.gd
#
# Writes user://terrain_lighting_m0.png and user://terrain_lighting_887.png
#   (~/Library/Application Support/Godot/app_userdata/<project>/).
# Override the pair with MERIDIAN_PROBE_OUT_DIR=/abs/dir to drop them somewhere handy.

extends SceneTree

const ZONE_DIR := "res://meridian/chibi/chunks/sprout_meadow"
const SPAWN := Vector3(-320.0, 0.0, -320.0)   # the server's kZoneSpawnXY placeholder

# The M0 flat-bootstrap lighting (world.gd before #887) — the "before" leg.
const M0 := {
	"sun_euler": Vector3(-55.0, 35.0, 0.0),
	"sun_color": Color(1.0, 1.0, 1.0),
	"sun_energy": 1.0,
	"shadows": false,
	"bg": Color(0.30, 0.40, 0.52),
	"ambient_color": Color(0.45, 0.45, 0.5),
	"ambient_energy": 0.7,
}

# #887's — kept in step with world.gd's _build_environment() by hand. The probe is a
# LOOKING tool, not a gate; world_lighting_verify.gd is what asserts the real scene.
const NOW := {
	"sun_euler": Vector3(-26.0, 35.0, 0.0),
	"sun_color": Color(1.0, 0.95, 0.85),
	"sun_energy": 1.9,
	"shadows": true,
	"bg": Color(0.45, 0.62, 0.85),
	"ambient_color": Color(0.42, 0.54, 0.72),
	"ambient_energy": 0.24,
}

var _light: DirectionalLight3D
var _we: WorldEnvironment


func _out_path(name: String) -> String:
	var dir := OS.get_environment("MERIDIAN_PROBE_OUT_DIR")
	if dir.is_empty():
		return "user://%s" % name
	return "%s/%s" % [dir.rstrip("/"), name]


func _initialize() -> void:
	print("meridian chibi meadow lighting A/B RENDER PROBE (#887)")
	await _run()
	quit(0)


func _build_terrain() -> int:
	var n := 0
	for cz in range(-1, 2):
		for cx in range(-1, 2):
			# n1_n1 / 0_0 / 1_1 … — the emitter's negative-cell naming.
			var fx: String = ("n%d" % -cx) if cx < 0 else str(cx)
			var fz: String = ("n%d" % -cz) if cz < 0 else str(cz)
			var path := "%s/%s_%s.tscn" % [ZONE_DIR, fx, fz]
			var ps := load(path) as PackedScene
			if ps == null:
				push_warning("chunk missing: %s" % path)
				continue
			# The chunk scenes carry their own world transform — they self-place.
			root.add_child(ps.instantiate())
			n += 1
	return n


# Ground height at an XZ, from the spawn chunk's nearest baked vertex. The chunks carry
# no collision (they're render meshes), and the mover's heightfield sampler is a whole
# streaming rig to stand up — for SITTING A PROBE CAPSULE ON THE GRASS, nearest-vertex
# on a 1 m lattice is within centimetres and needs nothing but the mesh. If this is
# skipped the capsule spawns at y=0 and sinks into a hill, which silently costs you the
# one thing the character is in this shot to show: whether its shadow grounds it.
func _ground_at(p: Vector3) -> float:
	var ps := load("%s/0_0.tscn" % ZONE_DIR) as PackedScene
	var mi := ps.instantiate() as MeshInstance3D
	var verts: PackedVector3Array = (mi.mesh as ArrayMesh).surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
	var off := mi.transform.origin
	var best := INF
	var best_y := 0.0
	for v in verts:
		var w := v + off
		var d := Vector2(w.x - p.x, w.z - p.z).length_squared()
		if d < best:
			best = d
			best_y = w.y
	return best_y


# A stand-in for the focal point: is the character still readable, and does its shadow
# ground it on the hills?
func _build_character(at: Vector3) -> void:
	var mi := MeshInstance3D.new()
	var cap := CapsuleMesh.new()
	cap.radius = 0.4
	cap.height = 1.8
	mi.mesh = cap
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.85, 0.25, 0.30)   # a chibi red "player"
	m.roughness = 0.9
	mi.mesh.surface_set_material(0, m)
	mi.position = at + Vector3(0.0, 0.9, 0.0)   # capsule origin is its centre
	root.add_child(mi)


func _apply(cfg: Dictionary) -> void:
	_light.rotation = Vector3(
		deg_to_rad(cfg["sun_euler"].x), deg_to_rad(cfg["sun_euler"].y), 0.0)
	_light.light_color = cfg["sun_color"]
	_light.light_energy = cfg["sun_energy"]
	_light.shadow_enabled = cfg["shadows"]
	_light.shadow_opacity = 0.75
	_light.light_angular_distance = 1.5
	_light.shadow_bias = 0.04
	_light.shadow_normal_bias = 2.0
	_light.directional_shadow_max_distance = 80.0
	var env: Environment = _we.environment
	env.background_color = cfg["bg"]
	env.ambient_light_color = cfg["ambient_color"]
	env.ambient_light_energy = cfg["ambient_energy"]


func _shoot(name: String) -> void:
	# Let the frame actually land on the GPU before reading it back.
	for _i in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	var img := root.get_texture().get_image()
	var path := _out_path(name)
	var err := img.save_png(path)
	print("  %s -> %s (%dx%d)" % [
		"wrote" if err == OK else "FAILED(%d)" % err, path, img.get_width(), img.get_height()])


func _run() -> void:
	root.set_size(Vector2i(1280, 720))

	var n := _build_terrain()
	print("  chunks instanced: %d (expect 9)" % n)

	_light = DirectionalLight3D.new()
	root.add_child(_light)
	_we = WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_we.environment = env
	root.add_child(_we)

	var player := SPAWN + Vector3(0.0, _ground_at(SPAWN), 0.0)
	print("  ground at spawn: %.2f m" % player.y)
	_build_character(player)

	# The framing has to be the one the PLAYER gets, or the picture proves nothing about
	# what they'll see: tps_camera_core.h's real rig — a 6 m boom (kZoomDefault) off a
	# chest-height pivot, pitched ~17° down. That is a GRAZING view of the ground, which
	# is exactly why a raking sun (and not a higher, "prettier" one) is the lever here; a
	# flattering top-down flyover would have made the M0 lighting look fine too.
	var pivot := player + Vector3(0.0, 1.5, 0.0)
	var pitch := deg_to_rad(17.0)
	var cam := Camera3D.new()
	cam.fov = 70.0
	cam.current = true
	# look_at_from_position, NOT position + look_at: _initialize() runs before the tree
	# is up, so nothing here is "inside tree" yet and look_at just errors — and it
	# errors NON-FATALLY, so the wrong call still renders, still exits 0, and silently
	# hands you an unaimed camera pointing at whatever -Z happens to be. Which is
	# precisely how a "visual" story ships a green run of the wrong picture.
	cam.look_at_from_position(
		pivot + Vector3(0.0, 6.0 * sin(pitch), 6.0 * cos(pitch)), pivot, Vector3.UP)
	root.add_child(cam)

	print("\n-- before: M0 flat-bootstrap lighting (sun 55°, ambient 0.70) --")
	_apply(M0)
	await _shoot("terrain_lighting_m0.png")

	print("\n-- after: #887 lighting (sun 26°, ambient 0.24, warm sun / cool fill) --")
	_apply(NOW)
	await _shoot("terrain_lighting_887.png")
	print("\nprobe done — LOOK AT THE TWO PNGs; this is not a pass/fail gate.")
