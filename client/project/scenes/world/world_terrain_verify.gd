# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for ENTER-WORLD TERRAIN STREAMING
# (issue #558, Epic #22 Story E — the finale). NOT a shipped scene: run under
#   MERIDIAN_ZONE_FIXTURE_DIR=<abs path to the checked-in chunkpack zone01 dir> \
#     godot --headless --path client/project \
#       --script res://scenes/world/world_terrain_verify.gd
# so CI / a dev box can prove, in the REAL engine (registered GDExtension classes,
# real ResourceLoader threaded load, real PackedScene instancing, real heightfield
# decode), with NO server and NO display, that the WHOLE chunk-streaming chain wires
# into the running world (Client SAD §2.3 Boot→mount→stream; §5.5 movement parity):
#
#   1. Fail-closed pack mount+verify (A/#554): the chunk index verifies CHUNK_OK on
#      the good pack and HARD-FAILS on a missing payload (a broken pack never enters).
#   2. Heightfield ground-sample (D/#557): the mover's ground backend, fed the shipped
#      `.chunk.bin`, samples REAL terrain height (not y=0) at the spawn, reports a
#      non-resident cell as NOT walkable, and HOLDS an entity over unloaded ground
#      (never drops it through — the #558 no-fall-through guarantee).
#   3. World integration: world.gd enters the streamed zone — a MeridianChunkStream
#      chunk root (B/C) replaces the flat bootstrap box, the loading screen holds
#      until the spawn chunks are resident then reveals, the player STANDS ON THE
#      STREAMED TERRAIN, and chunks stream/unload as a scripted player path moves.
#   4. Fail-closed at the scene level: a broken pack blocks enter-world (no terrain,
#      no spawn) instead of dropping the player onto a guessed map.
#
# It stages the CHECKED-IN Story-0 fixture pack (test/fixtures/chunkpack/…/zone01) into
# user:// (rewriting the IF-8 asset table's res:// prefix to the staged dir) so the
# streamer + verifier resolve real files. End-to-end against REAL Zone-01 content is
# gated on the content pipeline (Forge #26/#315) + #562 server-heightfield parity; this
# proves the CLIENT integration against the fixture. Exits 0 on success, 1 on any fail.

extends SceneTree

# Fixture zone geometry (tools/mcc/src/stages/chunk_emit.cpp; matches the manifest).
const ZONE_ORIGIN := -384.0
const CHUNK_SIZE := 128
# The spawn we drive the client to: the centre of chunk (0,0). Cell (0,0) spans world
# x/z [-384, -256], so its centre is -320. Inside the 3×3 grid — a valid spawn cell.
const SPAWN_CELL := Vector2i(0, 0)
const SPAWN := Vector3(-320.0, 0.0, -320.0)

const STAGE_GOOD := "user://zone01_verify"
const STAGE_BROKEN := "user://zone01_broken"
const RES_PREFIX := "res://meridian/core/chunks/zone01/"

# Every cell's payload triplet + the two manifests (the fixture's fixed file set).
const CELLS := ["n1_n1", "0_n1", "1_n1", "n1_0", "0_0", "1_0", "n1_1", "0_1", "1_1"]

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


# The analytic Story-0 fixture surface (chunk_emit.cpp fixture_height) — the ground
# height the heightfield MUST reproduce at a world XZ (used to assert spawn-on-terrain).
func _fixture_height(wx: float, wz: float) -> float:
	var grid_min_x := -512.0
	var center := -320.0
	var ramp := 0.08 * (wx - grid_min_x)
	var ndx := (wx - center) / float(CHUNK_SIZE)
	var ndz := (wz - center) / float(CHUNK_SIZE)
	return 8.0 + ramp + 2.5 * (ndx * ndx + ndz * ndz)


# --- Fixture staging ---------------------------------------------------------

func _stage_pack(src_dir: String, dst_dir: String, drop: String = "") -> bool:
	DirAccess.make_dir_recursive_absolute(dst_dir)
	var ok := true
	# Copy every payload (skip a dropped file to fabricate a broken pack). The fixture's
	# baked scenes are TEXT-format but carry the `.scn` extension, which binds Godot's
	# BINARY scene loader (it rejects them: "Unrecognized binary resource file"). Stage
	# them as `.tscn` so the TEXT loader handles them. The BYTES are unchanged, so the
	# manifest's per-chunk integrity hash (over the payload bytes) still matches — a
	# rename only; the real pipeline ships binary `.scn` the loader reads directly.
	for c in CELLS:
		for pair in [[".scn", ".tscn"], [".proxy.scn", ".proxy.tscn"], [".chunk.bin", ".chunk.bin"]]:
			var src_name := "%s%s" % [c, pair[0]]
			if src_name == drop:
				continue
			if not _copy_bytes("%s/%s" % [src_dir, src_name], "%s/%s%s" % [dst_dir, c, pair[1]]):
				ok = false
	# chunks.json rides by value (references chunks by id, no res:// paths to rewrite).
	if not _copy_bytes("%s/zone01.chunks.json" % src_dir, "%s/zone01.chunks.json" % dst_dir):
		ok = false
	# assets.json: rewrite the res:// chunk prefix to the staged dir AND the `.scn` scene
	# extension to `.tscn` (matching the renamed files) so the verifier + streamer resolve
	# them. `.chunk.bin` is untouched (it does not end in `.scn`).
	var assets := FileAccess.get_file_as_string("%s/zone01.assets.json" % src_dir)
	if assets.is_empty():
		ok = false
	else:
		assets = assets.replace(RES_PREFIX, "%s/" % dst_dir).replace(".scn\"", ".tscn\"")
		var f := FileAccess.open("%s/zone01.assets.json" % dst_dir, FileAccess.WRITE)
		if f == null:
			ok = false
		else:
			f.store_string(assets)
			f.close()
	return ok


func _copy_bytes(src: String, dst: String) -> bool:
	if not FileAccess.file_exists(src):
		return false
	var bytes := FileAccess.get_file_as_bytes(src)
	var f := FileAccess.open(dst, FileAccess.WRITE)
	if f == null:
		return false
	f.store_buffer(bytes)
	f.close()
	return true


func _initialize() -> void:
	print("meridian ENTER-WORLD TERRAIN STREAMING verify (#558)")

	# --- 0. Classes + the Story-E API surface load. ---
	for cls in ["MeridianChunkStream", "MeridianPackMount", "MeridianMovementController"]:
		_check("%s class registered" % cls, ClassDB.class_exists(cls))
	if _fails > 0:
		_finish()
		return
	var mover := MeridianMovementController.new()
	for m in ["use_heightfield_zone", "add_heightfield_chunk", "sample_ground",
			"has_ground_at", "is_heightfield_active", "heightfield_chunk_count"]:
		_check("mover API present: %s()" % m, mover.has_method(m))

	# --- 1. Stage the checked-in fixture into user://. ---
	var src := OS.get_environment("MERIDIAN_ZONE_FIXTURE_DIR")
	_check("MERIDIAN_ZONE_FIXTURE_DIR set", not src.is_empty()
		and FileAccess.file_exists("%s/zone01.chunks.json" % src))
	if src.is_empty():
		print("  set MERIDIAN_ZONE_FIXTURE_DIR to the checked-in chunkpack zone01 dir")
		_finish()
		return
	_check("staged good pack into user://", _stage_pack(src, STAGE_GOOD))
	# A broken pack: identical but missing chunk (0,0)'s baked scene payload.
	_check("staged broken pack (0_0.scn dropped)", _stage_pack(src, STAGE_BROKEN, "0_0.scn"))

	var good_chunks := "%s/zone01.chunks.json" % STAGE_GOOD
	var good_assets := "%s/zone01.assets.json" % STAGE_GOOD
	var broken_chunks := "%s/zone01.chunks.json" % STAGE_BROKEN
	var broken_assets := "%s/zone01.assets.json" % STAGE_BROKEN

	# ═══════════════════════════════════════════════════════════════════════════
	# 1. FAIL-CLOSED pack mount + verify (Story A, #554).
	# ═══════════════════════════════════════════════════════════════════════════
	print("\n-- Story A: fail-closed chunk-pack verify --")
	var pm := MeridianPackMount.new()
	var good: Dictionary = pm.verify_chunk_index(good_chunks, good_assets)
	_check("good pack verify -> ok", bool(good.get("ok", false)))
	_check("good pack verdict == CHUNK_OK",
		int(good.get("verdict", -1)) == MeridianPackMount.CHUNK_OK)
	_check("good pack verified all 9 chunks", int(good.get("verified_chunks", 0)) == 9)

	var pm2 := MeridianPackMount.new()
	var bad: Dictionary = pm2.verify_chunk_index(broken_chunks, broken_assets)
	_check("broken pack verify -> hard fail", bool(bad.get("hard_fail", false)) and not bool(bad.get("ok", true)))
	_check("broken pack verdict == CHUNK_MISSING_ASSET",
		int(bad.get("verdict", -1)) == MeridianPackMount.CHUNK_MISSING_ASSET)

	# ═══════════════════════════════════════════════════════════════════════════
	# 2. HEIGHTFIELD ground-sample + no-fall-through (Story D, #557).
	# ═══════════════════════════════════════════════════════════════════════════
	print("\n-- Story D: heightfield ground-sample + no-fall-through --")
	mover.use_heightfield_zone(ZONE_ORIGIN, ZONE_ORIGIN, float(CHUNK_SIZE))
	_check("mover heightfield backend active", mover.is_heightfield_active())

	# Before any chunk is resident the spawn cell is NOT walkable (out of bounds).
	_check("spawn NOT walkable before its chunk is resident", not mover.has_ground_at(SPAWN.x, SPAWN.z))

	# Feed the spawn chunk's shipped .chunk.bin -> the spawn now samples REAL terrain.
	var spawn_bin := FileAccess.get_file_as_bytes("%s/0_0.chunk.bin" % STAGE_GOOD)
	_check("read spawn chunk .chunk.bin", spawn_bin.size() > 0)
	_check("add_heightfield_chunk(0,0) decoded + resident",
		mover.add_heightfield_chunk(0, 0, spawn_bin) and mover.heightfield_chunk_count() == 1)

	var g: Dictionary = mover.sample_ground(SPAWN.x, SPAWN.z)
	var analytic := _fixture_height(SPAWN.x, SPAWN.z)   # ~23.36 m
	_check("spawn is now walkable", bool(g.get("walkable", false)))
	_check("spawn ground height is REAL terrain (not y=0)", float(g.get("height", 0.0)) > 10.0)
	_check("spawn ground height matches the analytic surface (~%.2f)" % analytic,
		abs(float(g.get("height", 0.0)) - analytic) < 0.25)

	# A neighbouring cell whose chunk is NOT resident is still not walkable.
	_check("non-resident neighbour cell is not walkable", not mover.has_ground_at(-200.0, -320.0))

	# NO FALL-THROUGH: seed the mover on the hill at a position whose chunk is NOT
	# resident (server-authoritative y) and predict idle — the y must HOLD, never
	# snap to the guessed y=0 plane.
	var hill := Vector3(-200.0, 25.0, -320.0)   # cell (1,0) — not fed
	mover.reset(hill, 0.0)
	for _i in range(20):
		mover.predict(Vector3.ZERO, false, false, 0.0, 1000 + _i * 50)
	var held: Vector3 = mover.get_predicted_position()
	_check("entity over non-resident ground holds its height (no fall-through)",
		abs(held.y - 25.0) < 0.5)

	# Spawn ON terrain: seed just above the surface at the spawn XZ (its chunk IS
	# resident) and let gravity settle the mover onto the heightfield over a few ticks.
	mover.reset(Vector3(SPAWN.x, analytic + 2.0, SPAWN.z), 0.0)
	for _i in range(120):
		mover.predict(Vector3.ZERO, false, false, 0.0, 5000 + _i * 50)
	var landed: Vector3 = mover.get_predicted_position()
	_check("player settles onto (stands on) the streamed terrain, not y=0",
		abs(landed.y - analytic) < 0.2 and mover.is_grounded())

	# ═══════════════════════════════════════════════════════════════════════════
	# 3. WORLD-SCENE integration: enter the streamed zone (the finale).
	# ═══════════════════════════════════════════════════════════════════════════
	print("\n-- Story E: world.gd enters the streamed zone --")
	await _verify_world_integration()

	# ═══════════════════════════════════════════════════════════════════════════
	# 4. FAIL-CLOSED at the scene level: a broken pack blocks enter-world.
	# ═══════════════════════════════════════════════════════════════════════════
	print("\n-- Story E: fail-closed enter-world (broken pack blocks spawn) --")
	await _verify_fail_closed_enter(broken_chunks, broken_assets)

	_finish()


func _verify_world_integration() -> void:
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		return
	var world := packed.instantiate()
	# Enter the streamed zone via the session handoff (production shape): dir + id +
	# spawn. No world_hello_frame -> offline (no socket), so this exercises the terrain
	# path with no server. _ready() runs _build_world_terrain() which mounts + streams.
	world.configure({"zone": {"dir": STAGE_GOOD, "id": "zone01", "spawn": SPAWN}}, {"name": "Verifier"})
	root.add_child(world)
	await _wait(3)

	_check("zone is active (streamed terrain, not flat bootstrap)", world.is_zone_active())
	_check("chunk-stream root built (replaces the flat ground box)",
		world.get_node_or_null("ChunkStream") != null)
	_check("NO flat bootstrap Ground box in the streamed zone",
		world.get_node_or_null("Ground") == null)
	_check("mover swapped to the heightfield backend",
		world._mover != null and world._mover.is_heightfield_active())

	# Drive physics frames until the loading screen reveals the world (spawn chunks in).
	var guard := 0
	while world.is_zone_loading() and guard < 900:
		await physics_frame
		guard += 1
	_check("loading screen revealed the world (spawn chunks resident)", not world.is_zone_loading())
	_check("loading screen overlay hidden after reveal",
		world._loading_screen != null and not world._loading_screen.is_active())

	var stream: MeridianChunkStream = world.get_chunk_stream()
	_check("spawn chunk instanced (mesh resident)",
		stream != null and stream.state_at(SPAWN_CELL.x, SPAWN_CELL.y) == MeridianChunkStream.STATE_INSTANCED)
	_check("streamer instanced chunks around the spawn", stream != null and stream.get_instanced_count() > 0)

	# THE headline proof: the local player STANDS ON THE STREAMED TERRAIN (heightfield
	# height at the spawn), not the y=0 flat plane.
	var player: Node3D = world.get_node_or_null("Player")
	var analytic := _fixture_height(SPAWN.x, SPAWN.z)
	_check("local player exists", player != null)
	if player != null:
		_check("player STANDS ON TERRAIN (y ~= %.2f, not 0)" % analytic,
			abs(player.position.y - analytic) < 0.6)

	# Stream as a scripted path moves: teleport the player toward cell (1,1) and prove
	# the streamer follows (new chunks stream in) AND the player keeps standing on the
	# terrain there (never falls through as it arrives ahead of / onto the new chunk).
	var target := Vector3(-160.0, 0.0, -160.0)   # cell (1,1) centre-ish
	var target_cell: Vector2i = stream.world_to_cell(target)
	world._mover.reset(Vector3(target.x, 100.0, target.z), 0.0)   # arrive high; ground unknown yet
	var moved_ok := true
	guard = 0
	while (stream.state_at(target_cell.x, target_cell.y) != MeridianChunkStream.STATE_INSTANCED
			or not world._mover.has_ground_at(target.x, target.z)) and guard < 900:
		# While the new chunk streams in, the player must NEVER be below the eventual
		# ground (no fall-through mid-stream): it holds until the heightfield arrives.
		if world._player != null and world._player.position.y < -5.0:
			moved_ok = false
		await physics_frame
		guard += 1
	_check("moving the player streamed in the destination chunk",
		stream.state_at(target_cell.x, target_cell.y) == MeridianChunkStream.STATE_INSTANCED)
	_check("destination heightfield resident (movement ground follows the stream)",
		world._mover.has_ground_at(target.x, target.z))
	_check("player never fell through the world while the chunk streamed in", moved_ok)

	# The player, resettled onto the destination, stands on THAT terrain height.
	var th := _fixture_height(target.x, target.z)
	var dg: Dictionary = world._mover.sample_ground(target.x, target.z)
	_check("destination samples its own terrain height (~%.2f, not 0)" % th,
		bool(dg.get("walkable", false)) and abs(float(dg.get("height", 0.0)) - th) < 0.25)

	# Moving far away recycles the origin chunks (streaming/unload as the player moves).
	var inst_before: int = stream.get_instanced_count()
	world._mover.reset(Vector3(1000.0, 0.0, 1000.0), 0.0)   # far outside the 3×3 grid
	guard = 0
	while stream.get_instanced_count() >= inst_before and guard < 300:
		await physics_frame
		guard += 1
	_check("leaving the zone recycled/unloaded chunks (streaming out)",
		stream.get_instanced_count() < inst_before)

	world.queue_free()
	await _wait(2)


func _verify_fail_closed_enter(broken_chunks: String, broken_assets: String) -> void:
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	var world := packed.instantiate()
	world.configure({}, {"name": "Verifier"})   # offline; enter the broken zone by hand
	root.add_child(world)
	await _wait(3)
	var res: Dictionary = world.enter_streamed_zone(broken_chunks, broken_assets, SPAWN, "zone01")
	_check("enter_streamed_zone(broken) -> ok == false (fail-closed)", not bool(res.get("ok", true)))
	_check("broken enter did NOT activate the zone", not world.is_zone_active())
	_check("broken enter built NO chunk-stream root (never spawns on a bad map)",
		world.get_node_or_null("ChunkStream") == null)
	world.queue_free()
	await _wait(2)


func _finish() -> void:
	print("\n%d checks, %d failures" % [_checks, _fails])
	if _fails == 0:
		print("[terrain] MERIDIAN_WORLD_TERRAIN_VERIFY_OK spawn_on_terrain no_fall_through")
		print("world terrain verify: ALL PASS")
	else:
		print("world terrain verify: %d FAILURE(S)" % _fails)
	quit(1 if _fails > 0 else 0)
