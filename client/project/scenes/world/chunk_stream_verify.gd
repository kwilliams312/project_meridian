# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the CHUNK STREAMER
# (issue #555, Epic #22 Story B). NOT a shipped scene: run under
#   godot --headless --script res://scenes/world/chunk_stream_verify.gd
# so CI / a dev box can prove, in the REAL engine (registered GDExtension class,
# real ResourceLoader threaded load, real PackedScene instancing, real Node pool),
# with NO server and NO display, that:
#   * the MeridianChunkStream GDExtension class loads + exposes its API;
#   * world→cell math + the desired ring per TIER radius (Q3: Low 3×3 / Med 5×5 /
#     Epic 7×7) are correct and the radii are config-overridable;
#   * driving a synthetic player position streams the desired chunks IN via real
#     load_threaded_request → instancing, honouring the per-frame instancing budget
#     (never exceeded), until every desired chunk is a child of the streamer;
#   * moving the player away RECYCLES the chunks into the pool (deferred unload,
#     not churn-free) and returning REUSES the pooled instances.
#
# Self-contained: it BUILDS tiny baked-mesh chunk scenes (a BoxMesh MeshInstance3D,
# the Q2 baked-`.scn` shape) into user:// at runtime and streams THOSE, so it ships
# no fixtures and touches no res:// content. The deterministic, engine-free proof of
# the same policy is client/gdextension/meridian/test/chunk_stream_core_test.cpp.
#
# Exits 0 on success, 1 on any failed assertion.

extends SceneTree

const ZONE_ORIGIN := -384.0
const CHUNK_SIZE := 128
const STAGE_DIR := "user://chunk_stream_verify"

var _fails := 0
var _checks := 0


func _check(name: String, ok: bool) -> void:
	_checks += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


# Cell (cx,cz) centre in world metres (origin is cell (0,0)'s min corner).
func _cell_centre(cx: int, cz: int) -> Vector3:
	var half := float(CHUNK_SIZE) * 0.5
	return Vector3(ZONE_ORIGIN + cx * CHUNK_SIZE + half, 0.0,
		ZONE_ORIGIN + cz * CHUNK_SIZE + half)


func _scene_name(cx: int, cz: int) -> String:
	var sx := ("n%d" % -cx) if cx < 0 else ("%d" % cx)
	var sz := ("n%d" % -cz) if cz < 0 else ("%d" % cz)
	return "%s_%s.scn" % [sx, sz]


# Build a tiny baked-mesh chunk scene (Q2 shape) and save it into user://.
func _bake_chunk_scene(path: String) -> bool:
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = Vector3(CHUNK_SIZE, 8.0, CHUNK_SIZE)
	mi.mesh = bm
	mi.name = "ChunkMesh"
	var ps := PackedScene.new()
	var perr := ps.pack(mi)
	var serr := OK
	if perr == OK:
		serr = ResourceSaver.save(ps, path)
	mi.free()
	return perr == OK and serr == OK


func _initialize() -> void:
	print("meridian chunk STREAMER runtime verify (#555)")

	# --- 1. The GDExtension class loads. ---
	_check("MeridianChunkStream class registered", ClassDB.class_exists("MeridianChunkStream"))
	if not ClassDB.class_exists("MeridianChunkStream"):
		quit(1)
		return

	# --- 2. Stage the 3×3 baked-mesh chunk scenes into user://. ---
	DirAccess.make_dir_recursive_absolute(STAGE_DIR)
	var chunks: Array = []
	var staged_ok := true
	for cz in range(-1, 2):
		for cx in range(-1, 2):
			var path := "%s/%s" % [STAGE_DIR, _scene_name(cx, cz)]
			if not _bake_chunk_scene(path):
				staged_ok = false
			chunks.append({
				"cx": cx, "cz": cz,
				"priority": 0 if (cx == 0 and cz == 0) else 1,
				"scene": path,
			})
	_check("staged 9 baked-mesh chunk scenes into user://", staged_ok)

	var stream := MeridianChunkStream.new()
	_check("MeridianChunkStream instantiated as Node3D", stream is Node3D)
	for m in ["configure", "set_player_position", "tick", "get_instanced_count",
			"get_desired_count", "get_active_radius", "set_tier", "set_tier_radius",
			"get_recycle_count", "get_pool_reuse_count", "get_last_instanced_this_tick"]:
		_check("  API present: %s()" % m, stream.has_method(m))
	root.add_child(stream)

	stream.configure({
		"origin_x": ZONE_ORIGIN, "origin_z": ZONE_ORIGIN, "chunk_size_m": CHUNK_SIZE,
		"min_cx": -1, "min_cz": -1, "max_cx": 1, "max_cz": 1,
		"chunks": chunks,
	})
	_check("configured 9-chunk zone", stream.get_chunk_count() == 9)

	# --- 3. world→cell + desired ring per tier (Q3 config). ---
	stream.set_tier(0)  # Low
	stream.set_player_position(_cell_centre(0, 0))
	_check("player cell is (0,0)", stream.get_player_cell() == Vector2i(0, 0))
	_check("Low radius == 1", stream.get_active_radius() == 1)
	_check("Low desired = 9 at centre", stream.get_desired_count() == 9)
	stream.set_tier(3)  # Epic
	_check("Epic radius == 3", stream.get_active_radius() == 3)
	stream.set_tier_radius(0, 0)  # override Low → radius 0 (config, not constant)
	stream.set_tier(0)
	_check("Low radius overridable to 0", stream.get_active_radius() == 0)
	_check("radius-0 desired = 1", stream.get_desired_count() == 1)
	stream.set_tier_radius(0, 1)  # restore Low = 1 ring
	stream.set_tier(0)

	# --- 4. Stream IN at centre with the REAL ResourceLoader; budget never exceeded. ---
	stream.set_instancing_budget(2)
	stream.set_player_position(_cell_centre(0, 0))
	var budget_ok := true
	var guard := 0
	while stream.get_instanced_count() < 9 and guard < 400:
		var inst: int = stream.tick()
		if inst > 2 or stream.get_last_instanced_this_tick() > 2:
			budget_ok = false
		await physics_frame
		guard += 1
	_check("per-frame instancing budget (2) never exceeded", budget_ok)
	_check("all 9 chunks streamed in (instanced)", stream.get_instanced_count() == 9)
	await _wait(3)  # flush deferred add_child
	_check("9 instances are children of the streamer", stream.get_child_count() == 9)

	# --- 5. Move away → pooled recycle; return → pooled reuse. ---
	var recy_before: int = stream.get_recycle_count()
	stream.set_player_position(Vector3(100000.0, 0.0, 100000.0))
	await _wait(1)
	for _i in range(8):
		stream.tick()
		await physics_frame
	_check("moving away recycled all 9 chunks", stream.get_recycle_count() - recy_before == 9)
	_check("nothing instanced away from the grid", stream.get_instanced_count() == 0)
	_check("9 instances pooled (not freed)", stream.get_pool_size() == 9)
	await _wait(3)  # flush deferred remove_child
	_check("streamer has no children away from the grid", stream.get_child_count() == 0)

	var reuse_before: int = stream.get_pool_reuse_count()
	stream.set_player_position(_cell_centre(0, 0))
	guard = 0
	while stream.get_instanced_count() < 9 and guard < 400:
		stream.tick()
		await physics_frame
		guard += 1
	_check("returning re-instanced all 9", stream.get_instanced_count() == 9)
	_check("re-instances REUSED the pool (reuse == 9)",
		stream.get_pool_reuse_count() - reuse_before == 9)
	_check("pool drained by reuse", stream.get_pool_size() == 0)

	# Deterministic teardown: detach + free the streamer subtree synchronously so no
	# instances leak at exit (a bare queue_free would not get an idle frame before quit).
	root.remove_child(stream)
	stream.free()

	print("\n%d checks, %d failures" % [_checks, _fails])
	if _fails == 0:
		print("[stream] MERIDIAN_STREAM_VERIFY_OK chunks=9 budget=2")
	quit(1 if _fails > 0 else 0)
