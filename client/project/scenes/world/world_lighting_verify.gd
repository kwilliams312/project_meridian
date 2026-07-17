# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the CHIBI MEADOW's LIGHTING
# (issue #887, epic #872 T5 — "the meadow reads flat because the shading doesn't
# reveal the relief"). NOT a shipped scene: run as
#
#   godot --headless --path client/project \
#     --script res://scenes/world/world_lighting_verify.gd
#
# The relief is real and correct — engine-measured ±2.4 m at ~5° slopes (#882/#884,
# and chibi_meadow_verify.gd re-proves it). The human E2E still read the meadow as a
# flat plane, because the M0-era lighting (sun 55° up, ambient energy 0.7) was built
# for a flat box that never had to shade. The epic is capped at ±3 m on purpose
# (worldd ground-samples a constant 0 and rejects |Δz| > 4 m — epic #874), so this is
# a LIGHTING fix, not an amplitude one, and this verify is what pins it down.
#
# What it proves, against the REAL world.tscn built offline (no server, no display):
#
#   1. The sun exists and is CONFIGURED for relief, not for a flat box: a grazing
#      mid-morning elevation (the dominant term for slope contrast), shadows on to
#      ground the character, warm daylight colour.
#   2. The ambient fill is LOW — a uniform fill adds the same value to every facet, so
#      it is the thing that was drowning the shape.
#   3. The ground material is NOT the flattener and still shades: chunk_emit's chibi
#      green is a real diffuse StandardMaterial3D (not unshaded), and the mesh carries
#      the per-vertex normals T1 bakes at full 129×129 res.
#   4. THE POINT: the meadow's gradient — MEASURED off the real mesh's baked normals,
#      not assumed — has materially more slope-to-slope contrast than the M0 baseline
#      this story replaces, and its visible crests clear a readable floor.
#   5. It didn't buy that contrast by going dark — IN THE LAMBERT MODEL. A chibi meadow
#      must stay BRIGHT, and "crush the ambient to zero" would ace check 4 while ruining
#      the look, so the modelled flat-ground luminance is held to a floor. Read the
#      caveat on this one: the model has NO SHADOW TERM, so this floor is blind to
#      anything the shadow pass does to the ground — which is precisely how #887's first
#      cut shipped a meadow 22% darker than M0 with this check green. The brightness
#      GUARANTEE is measured on real pixels by terrain_lighting_render_verify.gd; this
#      floor is only the cheap arithmetic net that catches the ambient-crush cheat.
#
# On "±2.4 m, ~5° slopes": that oversells it, and this verify deliberately does not
# repeat it. Measured off the staged chunk's own normals, ~5° is the p99 — the MEDIAN
# slope is ~1.8° and the chunk spans 3.47 m over 128 m (a ~2.7% grade). Assuming 5°
# would flatter the fix by ~3× at the angle most of the meadow actually is, so the
# contrast is evaluated at the measured percentiles instead.
#
# Checks 4/5 evaluate a first-order Lambert model (diffuse = albedo · (Σ light·N·L +
# ambient)) over the ACTUAL light/environment/material the scene builds — it is a
# model of the renderer, not the renderer, so it pins the numbers and catches
# regressions but is NOT the aesthetic proof. Whether the meadow READS as rolling is a
# human judgement: see the UI E2E hand-off on the PR, and scenes/world/
# terrain_lighting_render_verify.gd for the A/B render the eye actually judges.
#
# KNOW WHAT THIS MODEL CANNOT SEE. It has direct + ambient and nothing else — no shadow
# pass, no self-shadowing, no acne. Every artifact #887's first cut shipped lived in
# exactly that blind spot and this file was green throughout. Anything about SHADOWS is
# not knowable here and must be asserted on pixels in
# terrain_lighting_render_verify.gd; what this file can honestly do about them is bound
# the CONFIGURATION that provably drives them (phase 1's angular_distance band).
#
# Exits 0 on success, 1 on any fail.

extends SceneTree

# A real staged chibi chunk — the ground material + baked normals under test (#877).
const CHUNK_SCENE := "res://meridian/chibi/chunks/sprout_meadow/0_0.tscn"

# The M0 flat-bootstrap lighting this story replaces (world.gd before #887), mirrored
# here so the verify measures the improvement against a FIXED reference rather than
# reading it back from the thing under test.
const M0_SUN_ELEVATION_DEG := 55.0
const M0_SUN_COLOR := Color(1.0, 1.0, 1.0)
const M0_SUN_ENERGY := 1.0
const M0_AMBIENT_COLOR := Color(0.45, 0.45, 0.5)
const M0_AMBIENT_ENERGY := 0.7

# Thresholds, set from the MEASURED slope distribution (see the header). The lighting's
# contrast gain is a property of the light, not of any one slope, so it shows up as the
# same ratio at every percentile — that ratio is the honest headline, and 2× demands the
# fix is a real change rather than a nudge (the shipped numbers land ~3.5×).
const MIN_CONTRAST_VS_M0 := 2.0
# The p90 slopes are the ones that form the crests the eye actually reads as hills; they
# must clear a readable floor in absolute terms. (Deliberately NOT asserted at the
# median — 1.8° of ground cannot be made to read as a hill by any lighting, and a
# threshold pretending otherwise would just be a lie that happens to pass.)
const MIN_P90_SLOPE_CONTRAST := 0.15
# Chibi stays bright: the lit flat ground must not fall below this luminance. This is
# the guard that stops "more contrast" being bought with darkness.
const MIN_FLAT_LUMINANCE := 0.55
# The soft-shadow filter's radius. Bounded, NOT floored — see the check for why, and
# world.gd for the rendered sweep this number comes off. 0.1 is the largest value
# measured to keep shadow acne at the hard-shadow floor (0.024 vs 0.023 at 0.0).
const MAX_LIGHT_ANGULAR_DISTANCE := 0.1
# light_angular_distance is stored as a FLOAT32, so the 0.1 world.gd assigns reads back as
# 0.100000001490116…, which is > the float64 0.1 this file compares against. Without this
# epsilon the bound rejects its own shipped value — the check fails at exactly the setting
# it is there to bless. The epsilon is ~1e3 x the float32 gap and ~1e-3 x the distance to
# the nearest measured-bad value (0.15), so it cannot mask a real regression.
const LIGHT_ANGULAR_EPSILON := 1e-4
# A sun this shallow sculpts; much higher and the meadow's ~2° stops registering at all.
# (The M0 55° is what this rejects.)
const MAX_SUN_ELEVATION_DEG := 40.0
# ...but not so shallow that away-facing slopes black out and it stops reading as day.
const MIN_SUN_ELEVATION_DEG := 20.0
const MAX_AMBIENT_ENERGY := 0.4

var _fails := 0
var _checks := 0
# A GDScript runtime error inside an `await`ed method silently ABORTS the rest of the
# coroutine — the run would print "N/N passed" for however few it reached and exit 0,
# a FALSE GREEN. (#884's verify shipped exactly that bug; #875's winding bug passed
# every test while rendering nothing.) Each phase raises its flag as its LAST
# statement and _finish demands them all, so an early abort fails loudly.
var _scene_done := false
var _material_done := false
var _model_done := false


func _check(name: String, ok: bool) -> void:
	_checks += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _luminance(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b


# Lambert shade of a facet whose normal sits `slope_deg` off vertical, tilted within
# the sun's vertical plane (+ = tilted toward the sun). For a sun at elevation `e`,
# a vertical normal makes angle (90° - e) with the light, so tilting the facet toward
# it by `a` gives N·L = cos(90° - e - a) = sin(e + a).
func _shade(albedo: Color, sun_color: Color, sun_energy: float, sun_elev_deg: float,
		ambient_color: Color, ambient_energy: float, slope_deg: float) -> Color:
	var n_dot_l: float = maxf(0.0, sin(deg_to_rad(sun_elev_deg + slope_deg)))
	var out := Color.BLACK
	for i in range(3):
		var direct: float = sun_color[i] * sun_energy * n_dot_l
		var ambient: float = ambient_color[i] * ambient_energy
		out[i] = albedo[i] * (direct + ambient)
	return out


# The slope-to-slope contrast a ±slope_deg gradient produces: the luminance spread
# between a slope facing the sun and one facing away, over the flat-ground luminance.
# This is what the eye reads as "form".
func _slope_contrast(albedo: Color, sun_color: Color, sun_energy: float,
		sun_elev_deg: float, ambient_color: Color, ambient_energy: float,
		slope_deg: float) -> Dictionary:
	var flat := _luminance(_shade(albedo, sun_color, sun_energy, sun_elev_deg,
		ambient_color, ambient_energy, 0.0))
	var toward := _luminance(_shade(albedo, sun_color, sun_energy, sun_elev_deg,
		ambient_color, ambient_energy, slope_deg))
	var away := _luminance(_shade(albedo, sun_color, sun_energy, sun_elev_deg,
		ambient_color, ambient_energy, -slope_deg))
	var contrast: float = (toward - away) / flat if flat > 0.0 else 0.0
	return {"flat": flat, "toward": toward, "away": away, "contrast": contrast}


# The meadow's REAL gradient, read off the baked per-vertex normals: the angle each
# facet sits off vertical. Measured, never assumed — the headline "~5°" is the p99.
func _slope_percentiles(mesh: ArrayMesh) -> Dictionary:
	var normals: PackedVector3Array = mesh.surface_get_arrays(0)[Mesh.ARRAY_NORMAL]
	var angles: Array[float] = []
	for n in normals:
		angles.append(rad_to_deg(acos(clampf(n.normalized().y, -1.0, 1.0))))
	angles.sort()
	var sum := 0.0
	for a in angles:
		sum += a
	return {
		"count": angles.size(),
		"median": angles[angles.size() / 2],
		"p90": angles[int(angles.size() * 0.9)],
		"p99": angles[int(angles.size() * 0.99)],
		"mean": sum / float(angles.size()),
	}


func _find_child_of_type(node: Node, type_name: String) -> Node:
	for c in node.get_children():
		if c.is_class(type_name):
			return c
	return null


func _initialize() -> void:
	print("meridian chibi meadow LIGHTING verify (#887, epic #872 T5)")
	await _verify()
	_finish()


func _verify() -> void:
	# --- 1. Build the REAL world offline (no session → no socket), read its sun. ---
	print("\n-- 1. the sun world.tscn actually builds --")
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		return
	var world := packed.instantiate()
	_check("world instantiated as Node3D", world is Node3D)
	if not (world is Node3D):
		return
	root.add_child(world)
	await _wait(4)

	var light := _find_child_of_type(world, "DirectionalLight3D") as DirectionalLight3D
	_check("world builds a DirectionalLight3D (the sun)", light != null)
	if light == null:
		return

	# Elevation from the REAL basis, not the authored euler: the light travels along
	# its local -Z, so the vector TOWARD the sun is +Z, and its Y is sin(elevation).
	var to_sun: Vector3 = light.global_transform.basis.z.normalized()
	var sun_elev_deg: float = rad_to_deg(asin(clampf(to_sun.y, -1.0, 1.0)))
	print("     sun elevation = %.1f° (M0 was %.1f°)" % [sun_elev_deg, M0_SUN_ELEVATION_DEG])
	_check("sun is grazing enough to sculpt the meadow's ~2° ground (<= %.0f°)"
			% MAX_SUN_ELEVATION_DEG, sun_elev_deg <= MAX_SUN_ELEVATION_DEG)
	_check("sun is high enough to still read as daytime (>= %.0f°)" % MIN_SUN_ELEVATION_DEG,
		sun_elev_deg >= MIN_SUN_ELEVATION_DEG)
	_check("sun points DOWN at the ground (not up from below)", to_sun.y > 0.0)
	_check("sun is off-axis in azimuth (lights hills across the view)",
		absf(to_sun.x) > 0.05 or absf(to_sun.z) > 0.05)
	_check("sun casts shadows (grounds the character on the hills)", light.shadow_enabled)
	# This check used to read `light_angular_distance > 0.0` ("shadows are soft, not
	# harsh"), which was exactly backwards: it PASSED the 1.5 that broke the render and
	# would have FAILED the ~0 that fixes it. A verify that mandates the defect is worse
	# than no verify. What the pixels actually say (see world.gd's table, and
	# terrain_lighting_render_verify.gd, which re-measures it on every run): on near-flat
	# ground under a 26° sun, angular_distance buys nothing and costs everything — acne is
	# flat at ~0.023 up to 0.1, then climbs (0.15 -> 0.822, 0.20 -> 3.619, 0.25 -> 4.484),
	# and the character's grounding shadow smears away with it. So it is bounded, not
	# floored, and the ceiling is the largest value MEASURED to hold acne at the
	# hard-shadow baseline. Raising it means re-running the sweep, not editing the bound.
	print("     light_angular_distance = %.3f (M0 n/a — M0 had shadows off)"
		% light.light_angular_distance)
	_check("light_angular_distance is in the measured-clean band [0, %.2f] — a wider "
			% MAX_LIGHT_ANGULAR_DISTANCE
			+ "penumbra self-shadows this near-flat ground and destroys the character's "
			+ "grounding shadow",
		light.light_angular_distance >= 0.0
			and light.light_angular_distance
				<= MAX_LIGHT_ANGULAR_DISTANCE + LIGHT_ANGULAR_EPSILON)
	_check("shadows are not fully opaque (chibi shadow side stays open)",
		light.shadow_opacity > 0.0 and light.shadow_opacity < 1.0)
	# Near-flat ground raked by a low sun is the classic acne case.
	_check("shadow normal_bias set against acne on near-flat ground",
		light.shadow_normal_bias > 0.0)
	_check("sun is a warm daylight (R >= G >= B)",
		light.light_color.r >= light.light_color.g and light.light_color.g >= light.light_color.b)
	_check("sun is bright (energy >= 1.0, so low ambient doesn't dim the meadow)",
		light.light_energy >= 1.0)

	# --- 2. The ambient fill — the flattener — is turned down. ---
	print("\n-- 2. the ambient fill --")
	var we := _find_child_of_type(world, "WorldEnvironment") as WorldEnvironment
	_check("world builds a WorldEnvironment", we != null)
	if we == null or we.environment == null:
		_check("WorldEnvironment carries an Environment", false)
		return
	var env: Environment = we.environment
	print("     ambient energy = %.2f (M0 was %.2f)" % [env.ambient_light_energy, M0_AMBIENT_ENERGY])
	_check("ambient fill is low (<= %.2f) so it can't drown the shape" % MAX_AMBIENT_ENERGY,
		env.ambient_light_energy <= MAX_AMBIENT_ENERGY)
	_check("ambient fill is still present (shadow side never goes black)",
		env.ambient_light_energy > 0.0)
	_check("ambient is COOL (B > R) — warm sun vs cool fill is what shapes the hills",
		env.ambient_light_color.b > env.ambient_light_color.r)
	_check("sky reads as a bright chibi daytime blue",
		env.background_color.b > env.background_color.r
			and _luminance(env.background_color) > 0.35)
	_scene_done = true

	# --- 3. The ground material is not the flattener — it still shades. ---
	print("\n-- 3. the chibi ground material still shades --")
	var chunk_packed := load(CHUNK_SCENE) as PackedScene
	_check("a staged chibi chunk loads (%s)" % CHUNK_SCENE.get_file(), chunk_packed != null)
	if chunk_packed == null:
		return
	var chunk := chunk_packed.instantiate()
	root.add_child(chunk)
	var mi := chunk as MeshInstance3D
	_check("chunk is a MeshInstance3D", mi != null)
	if mi == null:
		return
	var mesh := mi.mesh
	_check("chunk carries an ArrayMesh (real terrain, not a placeholder box)",
		mesh is ArrayMesh)
	if not (mesh is ArrayMesh):
		return
	# The relief reads ENTIRELY from these — T1 bakes them at full 129×129 even though
	# the render mesh is stride-2, so the shading data is there to use.
	var fmt: int = (mesh as ArrayMesh).surface_get_format(0)
	_check("mesh carries per-vertex NORMALS (what the light has to work with)",
		(fmt & Mesh.ARRAY_FORMAT_NORMAL) != 0)
	var mat := (mesh as ArrayMesh).surface_get_material(0) as StandardMaterial3D
	_check("ground material is a StandardMaterial3D", mat != null)
	if mat == null:
		return
	# The suspicion in the story: is the material unshaded / flat-albedo in a way that
	# kills the gradient? It is not — it is a plain diffuse surface that responds to
	# the normals. It had nothing to reveal because the LIGHT gave it nothing.
	_check("ground material is SHADED (not SHADING_MODE_UNSHADED)",
		mat.shading_mode != BaseMaterial3D.SHADING_MODE_UNSHADED)
	_check("ground material takes light per-pixel",
		mat.shading_mode == BaseMaterial3D.SHADING_MODE_PER_PIXEL)
	_check("ground material is diffuse, not metal (metallic == 0)",
		is_equal_approx(mat.metallic, 0.0))
	_check("ground albedo is the saturated chibi green (G dominant)",
		mat.albedo_color.g > mat.albedo_color.r and mat.albedo_color.g > mat.albedo_color.b)
	_material_done = true

	# --- 4/5. THE POINT: does the meadow's REAL gradient read, and is it still bright? ---
	print("\n-- 4. slope-to-slope contrast at the meadow's MEASURED gradient --")
	var slopes := _slope_percentiles(mesh as ArrayMesh)
	print("     measured off %d baked normals: median=%.2f° mean=%.2f° p90=%.2f° p99=%.2f°"
		% [slopes["count"], slopes["median"], slopes["mean"], slopes["p90"], slopes["p99"]])
	print("     (the \"~5°\" headline is the p99 — most of the meadow is ~2°)")
	_check("the chunk really is gently sloped, not secretly steep (p99 < 10°)",
		slopes["p99"] < 10.0)

	var albedo: Color = mat.albedo_color
	var ratios: Array[float] = []
	var now_flat := 0.0
	var now_p90 := 0.0
	for key in ["median", "p90", "p99"]:
		var deg: float = slopes[key]
		var now := _slope_contrast(albedo, light.light_color, light.light_energy,
			sun_elev_deg, env.ambient_light_color, env.ambient_light_energy, deg)
		var m0 := _slope_contrast(albedo, M0_SUN_COLOR, M0_SUN_ENERGY,
			M0_SUN_ELEVATION_DEG, M0_AMBIENT_COLOR, M0_AMBIENT_ENERGY, deg)
		var ratio: float = now["contrast"] / m0["contrast"] if m0["contrast"] > 0.0 else 0.0
		ratios.append(ratio)
		print("     %-6s (±%.2f°): M0 %5.1f%%  ->  now %5.1f%%   (%.2fx)"
			% [key, deg, m0["contrast"] * 100.0, now["contrast"] * 100.0, ratio])
		now_flat = now["flat"]
		if key == "p90":
			now_p90 = now["contrast"]
		_check("±%.2f° slopes: sun-facing reads LIGHTER than away-facing (form, right way up)"
				% deg, now["toward"] > now["away"])
		_check("±%.2f° slopes: contrast >= %.0fx the M0 flat-bootstrap baseline"
				% [deg, MIN_CONTRAST_VS_M0], ratio >= MIN_CONTRAST_VS_M0)
	_check("the p90 crests clear the readable-contrast floor (%.0f%%)"
			% (MIN_P90_SLOPE_CONTRAST * 100.0), now_p90 >= MIN_P90_SLOPE_CONTRAST)

	print("\n-- 5. ...and it stays a BRIGHT chibi meadow (contrast not bought with dark) --")
	var m0_flat := _luminance(_shade(albedo, M0_SUN_COLOR, M0_SUN_ENERGY,
		M0_SUN_ELEVATION_DEG, M0_AMBIENT_COLOR, M0_AMBIENT_ENERGY, 0.0))
	print("     lit flat ground luminance = %.3f (M0 %.3f, floor %.2f)  [MODEL, no shadow term]"
		% [now_flat, m0_flat, MIN_FLAT_LUMINANCE])
	_check("lit flat ground holds the brightness floor IN THE LAMBERT MODEL (>= %.2f)"
			% MIN_FLAT_LUMINANCE, now_flat >= MIN_FLAT_LUMINANCE)
	# ⚠️ THIS FLOOR IS NOT THE BRIGHTNESS GUARANTEE, AND MUST NOT BE READ AS ONE.
	#
	# It binds correctly against the cheat it was written for: crushing ambient to 0 scores
	# MORE contrast (27.2%) but lands at 0.544 < 0.55 and is caught. But `_shade()` sums
	# direct + ambient and stops — THERE IS NO SHADOW TERM IN IT. Nothing the shadow pass
	# does to the ground is visible to this number, and the shadow pass is where #887's
	# real damage was: shipped at angular_distance 1.5 the terrain self-shadowed and the
	# RENDERED ground fell 0.678 -> 0.532 (-22%), while this model serenely reported 0.631
	# and passed. The verify passed a scene its own rule should have failed.
	#
	# So the model is kept — it is a cheap, display-free regression net on the light/ambient
	# arithmetic, and it is the thing that catches the ambient-crush cheat — but the
	# brightness GUARANTEE now lives where it can actually see the shadow pass:
	# terrain_lighting_render_verify.gd measures real pixels off a real GPU render.
	#
	# The model is only sound while the ground is NOT being self-shadowed, and that premise
	# is exactly what the angular_distance bound and normal_bias checks in phase 1 already
	# assert — they are not re-asserted here, because padding the count with duplicate
	# checks is how "40/40" stops meaning anything.
	print("     ^ MODEL only. It cannot see the shadow pass, so it is NOT the brightness")
	print("       guarantee: terrain_lighting_render_verify.gd measures the real pixels.")
	print("       It is sound here only because phase 1 bound angular_distance <= %.2f."
		% MAX_LIGHT_ANGULAR_DISTANCE)
	var deepest := _luminance(_shade(albedo, light.light_color, light.light_energy,
		sun_elev_deg, env.ambient_light_color, env.ambient_light_energy, -slopes["p99"]))
	_check("even the steepest away-facing slopes stay open, never crushed to black",
		deepest >= 0.30)
	_model_done = true


func _finish() -> void:
	# Demand every phase RAN — a silent coroutine abort must not read as green.
	_check("phase 1/2 (scene lighting) ran to completion", _scene_done)
	_check("phase 3 (ground material) ran to completion", _material_done)
	_check("phase 4/5 (contrast model) ran to completion", _model_done)
	print("\n%d/%d checks passed" % [_checks - _fails, _checks])
	if _fails > 0:
		print("LIGHTING VERIFY FAILED (%d)" % _fails)
		quit(1)
		return
	print("LIGHTING VERIFY OK")
	quit(0)
