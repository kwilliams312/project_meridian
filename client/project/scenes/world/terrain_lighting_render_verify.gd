# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — RENDERED-PIXEL verification of the chibi meadow's lighting
# (issue #887, epic #872 T5). This renders the REAL staged Sprout Meadow chunks under
# the M0 lighting and under the lighting world.gd ACTUALLY builds, writes both PNGs so
# a human can look at them, and — unlike world_lighting_verify.gd — ASSERTS on the
# pixels that come back.
#
# WHY THIS FILE ASSERTS, when it started life as a look-at-it-yourself probe:
#
# world_lighting_verify.gd evaluates a Lambert model of the shading. That model has
# direct + ambient and NOTHING ELSE — no shadow pass, no self-shadowing, no acne. #887's
# first cut set light_angular_distance = 1.5 ("soft shadows"), which on near-flat ground
# raked by a 26° sun makes the terrain shadow ITSELF: the rendered ground fell 0.678 ->
# 0.532 (-22%), pixel-scale acne rose ~100x, and the character's grounding shadow — the
# entire reason shadows are enabled — smeared into an unreadable sliver. The model saw
# none of it and reported a serene green, because every one of those artifacts lives in
# its blind spot. The brightness "floor" over there passed a scene it should have failed.
#
# So the brightness guarantee, and everything else about the shadow pass, is asserted
# HERE, on real pixels off a real GPU. A model cannot be the guarantee for something it
# structurally cannot see.
#
# Must run WINDOWED (a real GPU device) — `--headless` draws nothing, and on macOS the
# headless path aborts inside MoltenVK anyway (#283). Same as renderer_boot_verify.gd
# and login_screen_verify.gd, which also require a real adapter:
#
#   /Applications/Godot.app/Contents/MacOS/Godot --rendering-driver metal \
#     --path client/project --script res://scenes/world/terrain_lighting_render_verify.gd
#
# Writes user://terrain_lighting_m0.png and user://terrain_lighting_887.png
#   (~/Library/Application Support/Godot/app_userdata/<project>/).
# Override the pair with MERIDIAN_PROBE_OUT_DIR=/abs/dir to drop them somewhere handy.
# LOOK AT THEM. The numbers below pin the regressions that were actually hit; whether the
# meadow READS as a meadow is still a human judgement (see the UI E2E hand-off on the PR).
#
# Exits 0 on success, 1 on any fail.

extends SceneTree

const ZONE_DIR := "res://meridian/chibi/chunks/sprout_meadow"
const SPAWN := Vector3(-320.0, 0.0, -320.0)   # the server's kZoneSpawnXY placeholder
const WORLD_SCENE := "res://scenes/world/world.tscn"

# The M0 flat-bootstrap lighting (world.gd before #887) — the "before" leg, and the
# fixed reference the "after" leg is measured against. Frozen on purpose: it is history,
# so it is a literal here, unlike the "after" leg which is READ OUT OF world.gd.
const M0 := {
	"sun_euler": Vector3(-55.0, 35.0, 0.0),
	"sun_color": Color(1.0, 1.0, 1.0),
	"sun_energy": 1.0,
	"shadows": false,
	"shadow_opacity": 0.75,
	"angular_distance": 0.0,
	"shadow_bias": 0.04,
	"normal_bias": 2.0,
	"shadow_max_distance": 80.0,
	"bg": Color(0.30, 0.40, 0.52),
	"ambient_color": Color(0.45, 0.45, 0.5),
	"ambient_energy": 0.7,
}

# --- Thresholds. Every one is a MEASURED number, with the margin written next to it. ---
# Measured on this exact shot (3x3 staged chunks, real 6 m TPS boom, mid-ground band),
# macOS/Metal. See world.gd's _build_environment() for the sweep they come off.

# THE BRIGHTNESS GUARANTEE — the one world_lighting_verify.gd's model cannot make.
# Measured: M0 0.678, shipped-#887 0.643. The 1.5 bug scored 0.532 and must fail.
const MIN_GROUND_LUMINANCE := 0.60
# ...and it must not go dark RELATIVE to M0 either, which is the form the regression
# actually took. Measured: -5.2% (0.643 vs 0.678). The 1.5 bug was -21.6%.
const MAX_GROUND_DROP_VS_M0 := 0.10
# Shadow acne: mean |2nd difference| of luminance across neighbouring ground pixels, in
# %. Smooth Lambert shading contributes ~nothing. Measured: M0 0.007, shipped-#887 0.024
# (the hard-shadow floor is 0.023 — i.e. indistinguishable). The 1.5 bug scored 2.454,
# and angular_distance 0.15/0.20/0.25 score 0.822/3.619/4.484 — all must fail.
const MAX_ACNE := 0.5
# THE POINT OF ENABLING SHADOWS AT ALL: the character is grounded, not floating. Fraction
# of ground pixels near the capsule's base that are meaningfully darker than clean ground.
# Measured: M0 (shadows off) 0.00%, shipped-#887 11.76%. The 1.5 bug scored 1.76% — a
# smeared sliver, which is what "soft shadows" actually bought — and must fail.
const MIN_SHADOW_FRACTION := 0.05
# ...and it has to be a real shadow, not a faint smudge: darkest ground luminance near the
# base over clean ground luminance. Measured: M0 0.95, shipped-#887 0.54, the 1.5 bug 0.81.
const MAX_SHADOW_DEPTH_RATIO := 0.70

var _light: DirectionalLight3D
var _we: WorldEnvironment
var _fails := 0
var _checks := 0
# A GDScript runtime error inside an `await`ed method silently ABORTS the rest of the
# coroutine — the run would print "N/N passed" and exit 0, a FALSE GREEN (#884's verify
# shipped exactly that). Each phase raises its flag last and _finish demands them all.
var _m0_done := false
var _now_done := false


func _check(name: String, ok: bool) -> void:
	_checks += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _out_path(name: String) -> String:
	var dir := OS.get_environment("MERIDIAN_PROBE_OUT_DIR")
	if dir.is_empty():
		return "user://%s" % name
	return "%s/%s" % [dir.rstrip("/"), name]


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


# --- Pixel measurement ---------------------------------------------------------------

func _lum(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b


# The chibi ground is the saturated green; this rejects the red capsule and the blue sky
# without needing a depth prepass or a mask.
func _is_ground(c: Color) -> bool:
	return c.g > c.r and c.g > c.b


# Mean ground luminance over a band of the frame.
func _ground_luminance(img: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var n := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var c := img.get_pixel(x, y)
			if _is_ground(c):
				sum += _lum(c)
				n += 1
	return sum / float(n) if n > 0 else 0.0


# Median ground luminance of a clean reference band, well away from the character.
func _ref_ground_luminance(img: Image) -> float:
	var w := img.get_width()
	var h := img.get_height()
	var vals: Array[float] = []
	for y in range(int(h * 0.55), int(h * 0.75)):
		for x in range(int(w * 0.05), int(w * 0.25)):
			var c := img.get_pixel(x, y)
			if _is_ground(c):
				vals.append(_lum(c))
	if vals.is_empty():
		return 0.0
	vals.sort()
	return vals[vals.size() / 2]


# Pixel-scale luminance variation across the ground: mean |2·b − a − c| along x, in %.
# Smooth shading contributes ~nothing; shadow acne is exactly this signal.
func _acne(img: Image, x0: int, y0: int, x1: int, y1: int) -> float:
	var sum := 0.0
	var n := 0
	for y in range(y0, y1):
		for x in range(x0 + 1, x1 - 1):
			var a := img.get_pixel(x - 1, y)
			var b := img.get_pixel(x, y)
			var c := img.get_pixel(x + 1, y)
			if _is_ground(a) and _is_ground(b) and _is_ground(c):
				sum += absf(2.0 * _lum(b) - _lum(a) - _lum(c))
				n += 1
	return 100.0 * sum / float(n) if n > 0 else 0.0


# Is the character grounded? Looks at the ground around and up-sun of the capsule base
# and reports how much of it is in shadow, and how deep that shadow gets.
func _shadow_stats(img: Image) -> Dictionary:
	var w := img.get_width()
	var h := img.get_height()
	var ref := _ref_ground_luminance(img)
	var tot := 0
	var dark := 0
	var deepest := 1.0
	for y in range(int(h * 0.50), int(h * 0.72)):
		for x in range(int(w * 0.38), int(w * 0.58)):
			var c := img.get_pixel(x, y)
			if not _is_ground(c):
				continue
			tot += 1
			var l := _lum(c)
			deepest = minf(deepest, l)
			if l < 0.85 * ref:
				dark += 1
	return {
		"ref": ref,
		"fraction": float(dark) / float(tot) if tot > 0 else 0.0,
		"depth_ratio": deepest / ref if ref > 0.0 else 1.0,
	}


# --- Scene ---------------------------------------------------------------------------

# The "after" leg's lighting, READ OUT OF THE REAL world.tscn rather than copied here.
#
# This file used to carry a hand-maintained `NOW` dict with the note "kept in step with
# world.gd by hand". It was not: world.gd shipped angular_distance 1.5 and so did this
# copy, so the A/B render cheerfully confirmed the bug. A probe that duplicates the thing
# it is probing can only ever agree with it. The numbers now come from the scene under
# test, and drift is structurally impossible.
func _read_world_lighting() -> Dictionary:
	var packed := load(WORLD_SCENE) as PackedScene
	if packed == null:
		return {}
	var world := packed.instantiate()
	if not (world is Node3D):
		return {}
	# world.gd builds its sun/env in code (_build_environment), so the scene has to be in
	# the tree for a frame before there is anything to read. It builds them offline, with
	# no session and no socket — same as world_lighting_verify.gd relies on.
	root.add_child(world)
	await _wait(4)
	var light: DirectionalLight3D = null
	var we: WorldEnvironment = null
	for c in world.get_children():
		if c is DirectionalLight3D and light == null:
			light = c
		elif c is WorldEnvironment and we == null:
			we = c
	var cfg := {}
	if light != null and we != null and we.environment != null:
		var env: Environment = we.environment
		cfg = {
			# Read the REAL basis, not the authored euler: the light travels along its
			# local -Z, so +Z points at the sun and its Y is sin(elevation).
			"to_sun": light.global_transform.basis.z.normalized(),
			"sun_color": light.light_color,
			"sun_energy": light.light_energy,
			"shadows": light.shadow_enabled,
			"shadow_opacity": light.shadow_opacity,
			"angular_distance": light.light_angular_distance,
			"shadow_bias": light.shadow_bias,
			"normal_bias": light.shadow_normal_bias,
			"shadow_max_distance": light.directional_shadow_max_distance,
			"bg": env.background_color,
			"ambient_color": env.ambient_light_color,
			"ambient_energy": env.ambient_light_energy,
		}
	# Take the whole scene back out before rendering — its own camera and HUD must not end
	# up in our shot.
	root.remove_child(world)
	world.queue_free()
	await _wait(2)
	return cfg


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
	if cfg.has("to_sun"):
		# Aim the light back down the vector that points at the sun. (The node is in-tree
		# by now, but look_at_from_position is used for the same reason as the camera's.)
		var to_sun: Vector3 = cfg["to_sun"]
		_light.look_at_from_position(Vector3.ZERO, -to_sun, Vector3.UP)
	else:
		var e: Vector3 = cfg["sun_euler"]
		_light.rotation = Vector3(deg_to_rad(e.x), deg_to_rad(e.y), 0.0)
	_light.light_color = cfg["sun_color"]
	_light.light_energy = cfg["sun_energy"]
	_light.shadow_enabled = cfg["shadows"]
	_light.shadow_opacity = cfg["shadow_opacity"]
	_light.light_angular_distance = cfg["angular_distance"]
	_light.shadow_bias = cfg["shadow_bias"]
	_light.shadow_normal_bias = cfg["normal_bias"]
	_light.directional_shadow_max_distance = cfg["shadow_max_distance"]
	var env: Environment = _we.environment
	env.background_color = cfg["bg"]
	env.ambient_light_color = cfg["ambient_color"]
	env.ambient_light_energy = cfg["ambient_energy"]


func _shoot(name: String) -> Image:
	# Let the frame actually land on the GPU before reading it back.
	for _i in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	var img := root.get_texture().get_image()
	var path := _out_path(name)
	var err := img.save_png(path)
	print("  %s -> %s (%dx%d)" % [
		"wrote" if err == OK else "FAILED(%d)" % err, path, img.get_width(), img.get_height()])
	return img


func _measure(img: Image, label: String) -> Dictionary:
	var w := img.get_width()
	var h := img.get_height()
	# Mid-ground band: below the horizon, left of the capsule column.
	var d := {
		"lum": _ground_luminance(img, int(w * 0.05), int(h * 0.55), int(w * 0.45), int(h * 0.95)),
		"acne": _acne(img, int(w * 0.05), int(h * 0.55), int(w * 0.45), int(h * 0.95)),
	}
	d.merge(_shadow_stats(img))
	print("     %-3s  ground lum=%.4f  acne=%.3f  shadow=%.2f%%  deepest=%.2fx"
		% [label, d["lum"], d["acne"], d["fraction"] * 100.0, d["depth_ratio"]])
	return d


func _initialize() -> void:
	print("meridian chibi meadow lighting RENDERED-PIXEL verify (#887, epic #872 T5)")
	await _run()
	_finish()


func _run() -> void:
	# --- 0. The lighting under test, straight out of world.gd. ---
	print("\n-- 0. the lighting world.gd actually builds (read, not copied) --")
	var now: Dictionary = await _read_world_lighting()
	_check("read the real lighting out of %s" % WORLD_SCENE.get_file(), not now.is_empty())
	if now.is_empty():
		return
	var elev := rad_to_deg(asin(clampf((now["to_sun"] as Vector3).y, -1.0, 1.0)))
	print("     sun elev=%.1f°  energy=%.2f  angular_distance=%.3f  ambient=%.2f"
		% [elev, now["sun_energy"], now["angular_distance"], now["ambient_energy"]])

	var n := _build_terrain()
	print("  chunks instanced: %d (expect 9)" % n)
	_check("all 9 staged chunks instanced (the shot is of real terrain)", n == 9)

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
	# ...and PROVE it aimed, rather than trusting that it did.
	_check("camera is actually aimed at the player (no silent look_at abort)",
		cam.global_transform.origin.distance_to(pivot) > 5.0
			and (-cam.global_transform.basis.z).dot(
				(pivot - cam.global_transform.origin).normalized()) > 0.99)

	print("\n-- 1. before: M0 flat-bootstrap lighting (sun 55°, ambient 0.70, no shadows) --")
	_apply(M0)
	var m0_img := await _shoot("terrain_lighting_m0.png")
	var m0 := _measure(m0_img, "M0")
	_check("M0 leg renders ground at all (the shot isn't empty sky)", m0["lum"] > 0.0)
	# Sanity that the shadow metric measures what it claims: M0 has shadows OFF, so it
	# must report NO grounding shadow. If this ever "passes" the shadow check, the metric
	# is measuring something else and every number below it is worthless.
	_check("M0 (shadows off) shows NO grounding shadow — proves the metric sees shadows",
		m0["fraction"] < MIN_SHADOW_FRACTION)
	_m0_done = true

	print("\n-- 2. after: the lighting world.gd builds --")
	_apply(now)
	var now_img := await _shoot("terrain_lighting_887.png")
	var nw := _measure(now_img, "now")

	print("\n-- 3. THE BRIGHTNESS GUARANTEE, on pixels (what the model cannot see) --")
	_check("rendered ground holds the brightness floor (%.4f >= %.2f)"
			% [nw["lum"], MIN_GROUND_LUMINANCE], nw["lum"] >= MIN_GROUND_LUMINANCE)
	var drop: float = (m0["lum"] - nw["lum"]) / m0["lum"] if m0["lum"] > 0.0 else 1.0
	print("     rendered ground vs M0: %.4f vs %.4f  (%+.1f%%)"
		% [nw["lum"], m0["lum"], -drop * 100.0])
	_check("rendered ground didn't go dark vs M0 (drop %.1f%% <= %.0f%%)"
			% [drop * 100.0, MAX_GROUND_DROP_VS_M0 * 100.0], drop <= MAX_GROUND_DROP_VS_M0)

	print("\n-- 4. no shadow acne on the near-flat ground --")
	_check("ground is free of shadow acne (%.3f <= %.2f; M0 %.3f)"
			% [nw["acne"], MAX_ACNE, m0["acne"]], nw["acne"] <= MAX_ACNE)

	print("\n-- 5. the character is GROUNDED — the only reason shadows are on --")
	_check("the character casts a real grounding shadow (%.2f%% >= %.0f%% of nearby ground)"
			% [nw["fraction"] * 100.0, MIN_SHADOW_FRACTION * 100.0],
		nw["fraction"] >= MIN_SHADOW_FRACTION)
	_check("...and it reads as a shadow, not a smudge (%.2fx <= %.2fx clean ground)"
			% [nw["depth_ratio"], MAX_SHADOW_DEPTH_RATIO],
		nw["depth_ratio"] <= MAX_SHADOW_DEPTH_RATIO)
	_now_done = true


func _finish() -> void:
	# Demand every phase RAN — a silent coroutine abort must not read as green.
	_check("phase 1 (M0 leg) ran to completion", _m0_done)
	_check("phase 2-5 (lighting under test) ran to completion", _now_done)
	print("\n%d/%d checks passed" % [_checks - _fails, _checks])
	print("NOW LOOK AT THE TWO PNGs — these numbers pin the regressions that were")
	print("actually hit; whether the meadow reads as a meadow is still a human call.")
	if _fails > 0:
		print("TERRAIN LIGHTING RENDER VERIFY FAILED (%d)" % _fails)
		quit(1)
		return
	print("TERRAIN LIGHTING RENDER VERIFY OK")
	quit(0)
