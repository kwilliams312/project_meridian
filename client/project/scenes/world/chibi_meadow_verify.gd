# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for CHIBI'S VISIBLE TERRAIN
# (issue #877, epic #872 T3 — the story that makes terrain visible). NOT a shipped
# scene: run as
#
#   MERIDIAN_REALM_THEME=chibi godot --headless --path client/project \
#     --script res://scenes/world/chibi_meadow_verify.gd
#
# so CI / a dev box can prove, in the REAL engine (registered GDExtension classes,
# real ResourceLoader, real PackedScene instancing, real heightfield decode), with
# NO server and NO display, that Sprout Meadow actually renders rolling terrain.
#
# It differs from world_terrain_verify.gd (#558) in what it proves. That one drives
# the streamer against the checked-in Story-0 FIXTURE staged into user://, handed the
# zone via an explicit session handoff. This one takes NOTHING but the realm theme and
# proves the SHIPPING path end to end:
#
#   1. Zone-dir resolution (#877): MERIDIAN_REALM_THEME=chibi alone resolves the
#      STAGED res://meridian/chibi/chunks/sprout_meadow pack — no env zone override,
#      no session handoff, no chibi zone name hardcoded in client code.
#   2. The staged pack verifies CHUNK_OK (fail-closed mount, A/#554) straight off
#      res:// — the per-chunk BLAKE3 in the IF-6 manifest matches the staged payloads,
#      which is what check-golden's chunk gate keeps true.
#   3. `_zone_active` FLIPS: MeridianChunkStream streams the chunks and the M0 flat
#      bootstrap box is gone.
#   4. The chunks are real TERRAIN, not the pre-#875 placeholder: every instanced
#      chunk is an ArrayMesh carrying the chibi ground material (#877), no BoxMesh.
#   5. The character stands ON the hills: the mover's heightfield ground-sample at the
#      spawn is real relief (non-zero), the player's feet are on it, and the drawn
#      (stride-2) surface is within millimetres of it.
#   6. Movement is NOT rejected: every sampled height stays inside the server's R5
#      budget (|z - 0| <= 4 m against worldd's constant ground) — the whole reason
#      #872 ships with zero server changes.
#
# Exits 0 on success, 1 on any fail.

extends SceneTree

# Sprout Meadow's geometry (authored at zone01's — chunk_emit.h / #876), mirrored here
# so the verify asserts the numbers rather than reading them from the thing under test.
const SPAWN := Vector3(-320.0, 0.0, -320.0)   # the server's kZoneSpawnXY placeholder
const SPAWN_CELL := Vector2i(0, 0)
const ZONE_ORIGIN := -384.0
const CHUNK_SIZE := 128

# The server's own movement numbers (server/worldd/movement_constants.h): R5 rejects
# |proposed_z - ground(x,y)| > kHeightTolerance, and worldd's ground model is the
# CONSTANT plane z=0 (it cannot read a chunk — epic #874). So terrain height IS R5
# error budget, and the meadow is authored inside it.
const SERVER_HEIGHT_TOLERANCE := 4.0
const SERVER_FLAT_GROUND := 0.0
const MEADOW_BUDGET := 3.0          # the authored ±3 m budget (#876)

# What the theme resolution MUST land on (the staged pack check-golden gates).
const EXPECT_ZONE_ID := "sprout_meadow"
const EXPECT_ZONE_DIR := "res://meridian/chibi/chunks/sprout_meadow"

var _fails := 0
var _checks := 0
# A GDScript runtime error inside an `await`ed method silently ABORTS the rest of that
# coroutine: the run would report "N/N checks passed" for however few it reached and
# exit 0 — a FALSE GREEN (this bit during development, when sample_ground's Dictionary
# return aborted the run mid-way and it still printed PASS). Each phase raises its own
# flag as its LAST statement, and _finish demands them all, so an early abort fails.
var _staged_done := false
var _stream_done := false


# The mover's ground sample at a world XZ. sample_ground returns {height, walkable}
# (MeridianMovementController::sample_ground), NOT a bare float.
func _ground_height(mover, x: float, z: float) -> float:
	return float(mover.sample_ground(x, z).get("height", 0.0))


func _check(name: String, ok: bool, detail: String = "") -> void:
	_checks += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1
		if not detail.is_empty():
			print("        %s" % detail)


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _init() -> void:
	print("chibi Sprout Meadow — visible terrain verify (#877)\n")
	var theme := MeridianContentDB.resolve_theme()
	print("  realm theme: %s" % theme)
	if theme != "chibi":
		print("FAIL: run with MERIDIAN_REALM_THEME=chibi (got '%s')" % theme)
		quit(1)
		return
	await _verify_staged_pack()
	await _verify_theme_resolution_and_stream()
	_finish()


# --- 1/2. The staged pack verifies straight off res:// -----------------------

func _verify_staged_pack() -> void:
	print("\n-- staged chibi chunk pack (res://, as shipped) --")
	var chunks_json := "%s/%s.chunks.json" % [EXPECT_ZONE_DIR, EXPECT_ZONE_ID]
	var assets_json := "%s/%s.assets.json" % [EXPECT_ZONE_DIR, EXPECT_ZONE_ID]
	_check("staged IF-6 manifest present at %s" % chunks_json, FileAccess.file_exists(chunks_json))
	_check("staged IF-8 asset table present", FileAccess.file_exists(assets_json))
	# chunk-emit's own pack.manifest.json is deliberately NOT staged (#877): emit-pck
	# owns the namespace's pack manifest at the mount root. Two manifests for one
	# namespace is the "competing manifest" the #872 scout flagged.
	_check("chunk-emit's competing pack.manifest.json is NOT staged",
		not FileAccess.file_exists("%s/pack.manifest.json" % EXPECT_ZONE_DIR))
	_check("the namespace's real pack manifest IS at the mount root",
		FileAccess.file_exists("res://meridian/chibi/pack.manifest.json"))

	var mount := MeridianPackMount.new()
	var v: Dictionary = mount.verify_chunk_index(chunks_json, assets_json)
	_check("staged chunk pack verifies CHUNK_OK (fail-closed mount, integrity + presence)",
		int(v.get("verdict", -1)) == MeridianPackMount.CHUNK_OK,
		"verdict=%s reason=%s" % [v.get("verdict", -1), v.get("reason", "")])
	_check("manifest declares the chibi zone",
		String(v.get("zone", "")) == "chibi:zone.sprout_meadow",
		"zone=%s" % v.get("zone", ""))
	_check("all 9 chunks verified", int(v.get("verified_chunks", 0)) == 9,
		"verified=%d of %d" % [int(v.get("verified_chunks", 0)), int(v.get("chunk_count", 0))])

	# The chunk scenes are real terrain (#875) wearing the chibi ground (#877).
	var meshes_ok := true
	var mat_ok := true
	var detail := ""
	for cell in ["n1_n1", "0_n1", "1_n1", "n1_0", "0_0", "1_0", "n1_1", "0_1", "1_1"]:
		var scn := load("%s/%s.tscn" % [EXPECT_ZONE_DIR, cell]) as PackedScene
		if scn == null:
			meshes_ok = false
			detail = "%s.tscn failed to load" % cell
			continue
		var n := scn.instantiate()
		var mi := n as MeshInstance3D
		if mi == null or mi.mesh == null or not (mi.mesh is ArrayMesh) or mi.mesh.get_surface_count() != 1:
			meshes_ok = false
			detail = "%s is not a 1-surface ArrayMesh" % cell
			n.free()
			continue
		var mat := mi.mesh.surface_get_material(0) as StandardMaterial3D
		if mat == null:
			mat_ok = false
			detail = "%s has no StandardMaterial3D" % cell
		else:
			# The chibi look: a bright flat green with the specular highlight killed.
			if mat.albedo_color.g < mat.albedo_color.r or mat.albedo_color.g < mat.albedo_color.b:
				mat_ok = false
				detail = "%s ground is not green: %s" % [cell, mat.albedo_color]
			if mat.metallic_specular != 0.0 or mat.metallic != 0.0:
				mat_ok = false
				detail = "%s ground is not flat/matte (specular=%f metallic=%f)" % [cell, mat.metallic_specular, mat.metallic]
			if mat.resource_name != "chibi:art.terrain.sprout_meadow_ground":
				mat_ok = false
				detail = "%s material not keyed to the shared dep id: %s" % [cell, mat.resource_name]
		n.free()
	_check("every staged chunk scene is a real terrain ArrayMesh (no BoxMesh placeholder)",
		meshes_ok, detail)
	_check("every staged chunk wears the chibi ground material (flat bright green)",
		mat_ok, detail)
	_staged_done = true


# --- 3-6. The theme alone streams the meadow, and the player stands on it -----

func _verify_theme_resolution_and_stream() -> void:
	print("\n-- theme-derived zone resolution + streaming (the shipping path) --")
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		return
	var world := packed.instantiate()

	# THE #877 PROOF: configure with NO zone handoff and NO MERIDIAN_ZONE_DIR — the
	# realm theme alone must resolve Sprout Meadow. (world_terrain_verify passes an
	# explicit {"zone": {...}}; this deliberately does not.)
	#
	# Guard the premise first: an env zone override would take precedence in
	# _resolve_zone_paths and the "theme resolved it" checks below would pass without
	# the theme path ever running. Refuse to claim what we did not prove.
	_check("no MERIDIAN_ZONE_DIR/ID override in the environment (the theme path is "
		+ "what's under test)",
		OS.get_environment("MERIDIAN_ZONE_DIR").is_empty()
		and OS.get_environment("MERIDIAN_ZONE_ID").is_empty(),
		"MERIDIAN_ZONE_DIR=%s MERIDIAN_ZONE_ID=%s" % [OS.get_environment("MERIDIAN_ZONE_DIR"),
			OS.get_environment("MERIDIAN_ZONE_ID")])
	var resolved: Dictionary = world._resolve_zone_paths()
	_check("theme resolved the staged chibi zone dir",
		String(resolved.get("dir", "")) == EXPECT_ZONE_DIR,
		"dir=%s" % resolved.get("dir", ""))
	_check("theme discovered the zone id from the staged content (not hardcoded)",
		String(resolved.get("id", "")) == EXPECT_ZONE_ID,
		"id=%s" % resolved.get("id", ""))

	world.configure({}, {"name": "MeadowVerifier"})
	root.add_child(world)
	await _wait(3)

	_check("_zone_active FLIPPED — streaming chibi chunks, not the flat bootstrap",
		world.is_zone_active())
	_check("chunk-stream root built", world.get_node_or_null("ChunkStream") != null)
	_check("the M0 flat bootstrap Ground box is GONE", world.get_node_or_null("Ground") == null)
	_check("mover swapped to the heightfield backend",
		world._mover != null and world._mover.is_heightfield_active())

	# Hold until the spawn chunks are resident and the loading screen reveals.
	var guard := 0
	while world.is_zone_loading() and guard < 900:
		await physics_frame
		guard += 1
	_check("loading screen revealed the world (spawn chunks resident)", not world.is_zone_loading())

	var stream: MeridianChunkStream = world.get_chunk_stream()
	_check("spawn chunk instanced", stream != null
		and stream.state_at(SPAWN_CELL.x, SPAWN_CELL.y) == MeridianChunkStream.STATE_INSTANCED)
	_check("streamer instanced chunks around the spawn",
		stream != null and stream.get_instanced_count() > 0,
		"instanced=%d" % (stream.get_instanced_count() if stream != null else -1))

	# ---- The headline: the character stands ON the hills --------------------
	var mover = world._mover
	var ground: float = _ground_height(mover, SPAWN.x, SPAWN.z)
	_check("the mover ground-samples REAL relief at the spawn (not the flat 0 plane)",
		absf(ground - SERVER_FLAT_GROUND) > 0.25,
		"ground=%.4f m" % ground)
	_check("spawn ground sits inside the authored ±3 m meadow budget",
		absf(ground) <= MEADOW_BUDGET, "ground=%.4f m" % ground)

	var player: Node3D = world.get_node_or_null("Player")
	_check("local player exists", player != null)
	if player != null:
		_check("player STANDS ON THE TERRAIN (feet on the sampled ground, y=%.3f)" % player.position.y,
			absf(player.position.y - ground) < 0.6,
			"player.y=%.4f ground=%.4f" % [player.position.y, ground])

	# ---- R5: movement is not rejected --------------------------------------
	# Sample the whole walkable grid the way the server would see it: every metre of
	# terrain height spends R5 budget against worldd's constant ground 0. If ANY
	# resident sample breached 4 m, a player standing there would have every move
	# rejected. (Sampled at 4 m — dense enough to catch a breach on a 31 m wavelength.)
	var lo := INF
	var hi := -INF
	var breaches := 0
	var x := ZONE_ORIGIN - CHUNK_SIZE
	while x <= ZONE_ORIGIN + 2 * CHUNK_SIZE:
		var z := ZONE_ORIGIN - CHUNK_SIZE
		while z <= ZONE_ORIGIN + 2 * CHUNK_SIZE:
			if mover.has_ground_at(x, z):
				var h: float = _ground_height(mover, x, z)
				lo = minf(lo, h)
				hi = maxf(hi, h)
				if absf(h - SERVER_FLAT_GROUND) > SERVER_HEIGHT_TOLERANCE:
					breaches += 1
			z += 4.0
		x += 4.0
	var span := "span=[%.3f, %.3f] m" % [lo, hi]
	_check("every resident terrain sample stays inside the server's R5 tolerance "
		+ "(|z-0| <= 4 m) — moves are NOT rejected", breaches == 0,
		"%s breaches=%d" % [span, breaches])
	_check("the streamed meadow is genuinely rolling (real hills AND hollows about 0)",
		lo < -0.5 and hi > 0.5, span)
	_check("the streamed meadow stays inside the authored ±3 m budget", absf(lo) <= MEADOW_BUDGET
		and absf(hi) <= MEADOW_BUDGET, span)

	# ---- The stride decision's payoff --------------------------------------
	# The DRAWN surface is stride-2 while the mover samples the FULL-res heightfield,
	# so the character's feet and the visible ground can disagree. Measure that gap on
	# the REAL instanced meshes.
	#
	# Probing the mesh's own vertices would be vacuous — a retained vertex sits exactly
	# on its heightfield sample by construction (max dev would read 0.0000 and prove
	# nothing about the stride). The gap lives BETWEEN vertices, where the flat triangle
	# spans a sample the decimation dropped. So probe EDGE MIDPOINTS: the drawn surface
	# there is the average of the two endpoints, and the truth is the mover's sample.
	var chunk_root := world.get_node_or_null("ChunkStream")
	var probed := 0
	var vert_dev := 0.0
	var max_dev := 0.0
	# The stride is DERIVED from the mesh the emitter actually shipped, never spelled
	# here: it is a property of the profile's SurfaceStyle (mcc), so hardcoding "2" in
	# this report would quietly start lying the day a profile re-strides — and a verify
	# that misreports what it measured is worse than no verify. The heightfield is
	# 128 quads across, so a drawn grid of n verts/axis means stride = 128 / (n - 1).
	var drawn_stride := 0
	if chunk_root != null:
		for child in chunk_root.get_children():
			var mi := _first_mesh_instance(child)
			if mi == null:
				continue
			var arrays := (mi.mesh as ArrayMesh).surface_get_arrays(0)
			var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
			var org: Vector3 = mi.global_transform.origin
			if drawn_stride == 0:
				var per_axis := int(round(sqrt(float(verts.size()))))
				if per_axis > 1 and 128 % (per_axis - 1) == 0:
					drawn_stride = 128 / (per_axis - 1)
			# Walk triangle edges, sampling a scattering of them.
			var step: int = maxi(3, (idx.size() / 3 / 48) * 3)
			var t := 0
			while t + 2 < idx.size():
				for e in [[0, 1], [1, 2], [2, 0]]:
					var a: Vector3 = verts[idx[t + e[0]]]
					var b: Vector3 = verts[idx[t + e[1]]]
					# The vertex endpoints must be exactly on the heightfield ...
					for v in [a, b]:
						if mover.has_ground_at(org.x + v.x, org.z + v.z):
							vert_dev = maxf(vert_dev,
								absf(_ground_height(mover, org.x + v.x, org.z + v.z) - v.y))
					# ... and the drawn edge midpoint is where decimation actually costs.
					var m: Vector3 = (a + b) * 0.5
					var wx: float = org.x + m.x
					var wz: float = org.z + m.z
					if mover.has_ground_at(wx, wz):
						max_dev = maxf(max_dev, absf(_ground_height(mover, wx, wz) - m.y))
						probed += 1
				t += step
	_check("probed the instanced meshes' drawn surface against the heightfield",
		probed > 0, "probed=%d edge midpoints" % probed)
	_check("every drawn VERTEX sits exactly on the sampled heightfield (max dev %.4f m)"
		% vert_dev, vert_dev < 0.001, "vert_dev=%.5f m" % vert_dev)
	_check("derived the drawn mesh's stride from the emitted vertex count (stride %d)"
		% drawn_stride, drawn_stride > 0, "could not derive a stride from the mesh")
	_check("the DRAWN stride-%d surface meets the sampled ground BETWEEN vertices "
		% drawn_stride
		+ "(max dev %.4f m < 5 cm — imperceptible float/sink)" % max_dev,
		max_dev < 0.05, "max_dev=%.4f m over %d edge midpoints" % [max_dev, probed])
	_stream_done = true


func _first_mesh_instance(n: Node) -> MeshInstance3D:
	if n is MeshInstance3D and (n as MeshInstance3D).mesh is ArrayMesh:
		return n as MeshInstance3D
	for c in n.get_children():
		var found := _first_mesh_instance(c)
		if found != null:
			return found
	return null


func _finish() -> void:
	print("\n%d/%d checks passed" % [_checks - _fails, _checks])
	# A run that did not REACH the end of every phase is a failure, not a pass — an
	# awaited runtime error swallows the rest of the coroutine silently.
	if not (_staged_done and _stream_done):
		print("FAIL: the run ABORTED EARLY (staged_done=%s stream_done=%s) — a runtime "
			% [_staged_done, _stream_done]
			+ "error in an awaited call swallowed the rest. Scroll up for SCRIPT ERROR.")
		quit(1)
		return
	if _fails > 0:
		print("FAIL: %d check(s) failed" % _fails)
		quit(1)
	else:
		print("PASS — Sprout Meadow streams a rolling chibi meadow, and the character stands on it.")
		quit(0)
