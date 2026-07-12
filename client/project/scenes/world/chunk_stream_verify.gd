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


func _proxy_name(cx: int, cz: int) -> String:
	var sx := ("n%d" % -cx) if cx < 0 else ("%d" % cx)
	var sz := ("n%d" % -cz) if cz < 0 else ("%d" % cz)
	return "%s_%s.proxy.scn" % [sx, sz]


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

	root.remove_child(stream)
	stream.free()

	# ════════════════════════════════════════════════════════════════════════════
	#  Story C (#556): proxy far-ring + gapless swap + hitch gate — real engine.
	# ════════════════════════════════════════════════════════════════════════════
	await _verify_proxy_far_ring()

	print("\n%d checks, %d failures" % [_checks, _fails])
	if _fails == 0:
		print("[stream] MERIDIAN_STREAM_VERIFY_OK chunks=9 budget=2 proxy=1")
	quit(1 if _fails > 0 else 0)


# Story C: a 5×5 zone with baked proxies (one chunk explicitly proxy-less) streamed
# through the REAL MeridianChunkStream — proves the far-ring band, the gapless
# proxy↔full swap, the ≤50 ms hitch gate, and per-representation pooling.
func _verify_proxy_far_ring() -> void:
	print("\n-- Story C: proxy far-ring + hitch gate --")
	const NULL_CX := 2
	const NULL_CZ := 2

	# Stage 5×5 full scenes + proxies into user:// ((2,2) gets NO proxy = null/C3).
	var chunks: Array = []
	var staged_ok := true
	for cz in range(-2, 3):
		for cx in range(-2, 3):
			var spath := "%s/%s" % [STAGE_DIR, _scene_name(cx, cz)]
			if not _bake_chunk_scene(spath):
				staged_ok = false
			var entry := {
				"cx": cx, "cz": cz,
				"priority": 0 if (cx == 0 and cz == 0) else 1,
				"scene": spath,
			}
			if not (cx == NULL_CX and cz == NULL_CZ):
				var ppath := "%s/%s" % [STAGE_DIR, _proxy_name(cx, cz)]
				if not _bake_chunk_scene(ppath):
					staged_ok = false
				entry["proxy"] = ppath
			chunks.append(entry)
	_check("C: staged 5×5 scenes + proxies (one proxy:null)", staged_ok)

	var s := MeridianChunkStream.new()
	for m in ["set_tier_far_ring", "get_active_far_ring", "shown_rep_at",
			"get_proxy_instanced_count", "get_full_instanced_count", "get_swap_count",
			"set_instance_cost_ms", "budget_for_hitch_gate", "get_last_stream_frame_cost_ms",
			"get_proxy_desired_count"]:
		_check("C: API present: %s()" % m, s.has_method(m))
	root.add_child(s)
	s.configure({
		"origin_x": ZONE_ORIGIN, "origin_z": ZONE_ORIGIN, "chunk_size_m": CHUNK_SIZE,
		"min_cx": -2, "min_cz": -2, "max_cx": 2, "max_cz": 2,
		"chunks": chunks,
	})
	_check("C: configured 25-chunk zone", s.get_chunk_count() == 25)

	# Per-tier proxy far-ring is CONFIG (Q3: Low 3 / Med 4 / Epic 6).
	s.set_tier(MeridianChunkStream.TIER_LOW)
	_check("C: Low far-ring default == 3", s.get_active_far_ring() == 3)
	s.set_tier(MeridianChunkStream.TIER_EPIC)
	_check("C: Epic far-ring default == 6", s.get_active_far_ring() == 6)
	# Narrow Low to a clean full(1) + proxy(2) split over the 5×5 grid.
	s.set_tier(MeridianChunkStream.TIER_LOW)
	s.set_tier_far_ring(MeridianChunkStream.TIER_LOW, 2)
	_check("C: Low far-ring overridable to 2", s.get_active_far_ring() == 2)

	s.set_player_position(_cell_centre(0, 0))
	_check("C: full desired = 9 at centre", s.get_desired_count() == 9)
	_check("C: proxy desired = 15 (16 band − 1 null)", s.get_proxy_desired_count() == 15)

	# --- Stream in: full inside the radius, proxy in the far-ring, null shows nothing. ---
	s.set_instancing_budget(4)
	var guard := 0
	while s.get_instanced_count() < 24 and guard < 400:
		s.tick()
		await physics_frame
		guard += 1
	_check("C: 9 FULL chunks instanced inside the radius", s.get_full_instanced_count() == 9)
	_check("C: 15 PROXY chunks instanced in the far-ring", s.get_proxy_instanced_count() == 15)
	_check("C: (0,0) shows FULL", s.shown_rep_at(0, 0) == MeridianChunkStream.REP_FULL)
	_check("C: (2,0) shows PROXY", s.shown_rep_at(2, 0) == MeridianChunkStream.REP_PROXY)
	_check("C: (2,2) shows NOTHING (proxy:null)",
		s.shown_rep_at(NULL_CX, NULL_CZ) == MeridianChunkStream.REP_NONE)
	await _wait(3)
	_check("C: 24 instances are children (proxy + full)", s.get_child_count() == 24)

	# --- Gapless proxy↔full swap as the player steps across the boundary. ---
	var swaps_before: int = s.get_swap_count()
	s.set_player_position(_cell_centre(1, 0))
	_check("C: (2,0) now wants FULL", s.desired_rep_at(2, 0) == MeridianChunkStream.REP_FULL)
	_check("C: (-1,0) now wants PROXY", s.desired_rep_at(-1, 0) == MeridianChunkStream.REP_PROXY)
	var always_visible := true
	guard = 0
	while (s.shown_rep_at(2, 0) != MeridianChunkStream.REP_FULL
			or s.shown_rep_at(-1, 0) != MeridianChunkStream.REP_PROXY) and guard < 200:
		s.tick()
		if s.shown_rep_at(2, 0) == MeridianChunkStream.REP_NONE:
			always_visible = false
		if s.shown_rep_at(-1, 0) == MeridianChunkStream.REP_NONE:
			always_visible = false
		await physics_frame
		guard += 1
	_check("C: (2,0) swapped PROXY→FULL", s.shown_rep_at(2, 0) == MeridianChunkStream.REP_FULL)
	_check("C: (-1,0) swapped FULL→PROXY", s.shown_rep_at(-1, 0) == MeridianChunkStream.REP_PROXY)
	_check("C: neither chunk went invisible during the swap", always_visible)
	_check("C: swap counter advanced by >= 2", s.get_swap_count() - swaps_before >= 2)

	# --- Hitch gate: shared proxy+full budget; frame cost never exceeds 50 ms. ---
	_check("C: budget_for_hitch_gate(12.5) == 4", s.budget_for_hitch_gate(12.5) == 4)
	s.set_instance_cost_ms(12.5)
	var budget: int = s.budget_for_hitch_gate(12.5)
	s.set_instancing_budget(budget)
	# Teleport away then back → a burst that re-streams everything at once.
	s.set_player_position(Vector3(100000.0, 0.0, 100000.0))
	for _i in range(10):
		s.tick()
		await physics_frame
	_check("C: nothing shown away from the grid", s.get_instanced_count() == 0)
	var reuse_before: int = s.get_pool_reuse_count()
	s.set_player_position(_cell_centre(0, 0))
	var gate_ok := true
	var budget_ok := true
	guard = 0
	while s.get_instanced_count() < 24 and guard < 400:
		s.tick()
		if s.get_last_instanced_this_tick() > budget:
			budget_ok = false
		if s.get_last_stream_frame_cost_ms() > 50.0:
			gate_ok = false
		await physics_frame
		guard += 1
	_check("C: per-frame instancing budget (4) never exceeded", budget_ok)
	_check("C: streaming frame cost never exceeded the 50 ms gate", gate_ok)
	_check("C: everything re-streamed after the burst", s.get_instanced_count() == 24)
	_check("C: re-instances REUSED the pool (proxy + full pooled)",
		s.get_pool_reuse_count() - reuse_before == 24)

	# Flush any in-flight deferred add_child/remove_child before freeing so no
	# instance is orphaned at exit (deterministic teardown).
	await _wait(3)
	root.remove_child(s)
	s.free()
