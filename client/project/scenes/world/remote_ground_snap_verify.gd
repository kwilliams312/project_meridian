# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for REMOTE-ENTITY GROUND SNAP
# (issue #885, Epic #872 T4). NOT a shipped scene: run under
#   godot --headless --path client/project \
#     --script res://scenes/world/remote_ground_snap_verify.gd
# (MERIDIAN_ZONE_FIXTURE_DIR overrides the checked-in fixture pack.)
#
# WHY THIS EXISTS
# The terrain is CLIENT-SIDE ONLY: worldd's ground model is still the constant
# `kFlatGroundZ = 0` (server/worldd/movement_validation.cpp), so EVERY entity's
# wire height arrives as 0. The local player looks right because the client
# ground-samples it (D/#557). Remote entities got no such treatment — world.gd
# wrote the raw interpolated wire position straight to the node — so NPCs and
# other players sank into the meadow wherever it rises (up to +2.37 m). Human
# E2E of T3/#884 found them buried.
#
# WHAT IT PROVES (in the REAL engine: registered GDExtension classes, real
# heightfield decode, real world.tscn, no server, no display):
#   1. Zone mounted: a remote at wire height 0 over terrain of height h renders
#      at ~h — the SAME height the heightfield reports, not 0.
#   2. x/z are untouched — only the height is resolved from terrain, so the
#      interpolator's smoothing is preserved (no jitter on slopes).
#   3. Ground with no meaningful height (cell not streamed in / a hole) holds the
#      wire height instead of slamming the entity to 0 (no fall-through).
#   4. Flat bootstrap (no zone mounted): the render height is the wire height,
#      UNCHANGED — the snap is a strict no-op in the fallback.
#
# Exits 0 on success, 1 on any fail. Prints a completion sentinel: a run that
# dies mid-await prints no sentinel, so a truncated run can never read as green.

extends SceneTree

# Fixture zone geometry (tools/mcc/src/stages/chunk_emit.cpp; matches the manifest).
const ZONE_ORIGIN := -384.0
const CHUNK_SIZE := 128
const SPAWN_CELL := Vector2i(0, 0)
const SPAWN := Vector3(-320.0, 0.0, -320.0)

const STAGE_GOOD := "user://zone01_snap_verify"
const RES_PREFIX := "res://meridian/core/chunks/zone01/"

const CELLS := ["n1_n1", "0_n1", "1_n1", "n1_0", "0_0", "1_0", "n1_1", "0_1", "1_1"]

# Kinds mirrored from remote_interpolation.h (SampleKind).
const KIND_EMPTY := 0

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
# the heightfield MUST reproduce at a world XZ. Deliberately NOT flat and NOT 0:
# at the spawn it is ~23.36 m, so "snapped to terrain" and "left at wire 0" are
# nowhere near each other and no assertion here can pass vacuously.
func _fixture_height(wx: float, wz: float) -> float:
	var grid_min_x := -512.0
	var center := -320.0
	var ramp := 0.08 * (wx - grid_min_x)
	var ndx := (wx - center) / float(CHUNK_SIZE)
	var ndz := (wz - center) / float(CHUNK_SIZE)
	return 8.0 + ramp + 2.5 * (ndx * ndx + ndz * ndz)


# --- Fixture staging (mirrors world_terrain_verify.gd) ------------------------

func _stage_pack(src_dir: String, dst_dir: String) -> bool:
	DirAccess.make_dir_recursive_absolute(dst_dir)
	var ok := true
	for c in CELLS:
		for ext in [".tscn", ".proxy.tscn", ".chunk.bin"]:
			var n := "%s%s" % [c, ext]
			if not _copy_bytes("%s/%s" % [src_dir, n], "%s/%s" % [dst_dir, n]):
				ok = false
	if not _copy_bytes("%s/zone01.chunks.json" % src_dir, "%s/zone01.chunks.json" % dst_dir):
		ok = false
	var assets := FileAccess.get_file_as_string("%s/zone01.assets.json" % src_dir)
	if assets.is_empty():
		ok = false
	else:
		assets = assets.replace(RES_PREFIX, "%s/" % dst_dir)
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


# Spawn a remote entity node + feed the interpolator ONE snapshot at `wire`, the
# position as it arrives off the wire (server z-up -> Godot y-up already applied by
# the net thread's to_godot(); the height rides in .y). Returns the node.
func _add_remote(world: Node, guid: int, wire: Vector3) -> Node3D:
	var d := {"position": wire, "name": "Remote%d" % guid, "char_class": 0}
	world._spawn_remote(guid, d)
	world._interp.on_entity_enter(guid, wire, 0.0, world._client_ms)
	return world._remote_nodes.get(guid) as Node3D


func _initialize() -> void:
	print("meridian REMOTE-ENTITY GROUND SNAP verify (#885)")

	for cls in ["MeridianChunkStream", "MeridianMovementController", "MeridianRemoteInterpolator"]:
		_check("%s class registered" % cls, ClassDB.class_exists(cls))
	if _fails > 0:
		# A worktree whose .godot/global_script_class_cache.cfg was never seeded fails
		# here rather than silently loading nothing and "passing".
		print("  GDExtension classes missing — seed the import cache (godot --editor --quit --path client/project)")
		_finish()
		return

	var src := OS.get_environment("MERIDIAN_ZONE_FIXTURE_DIR")
	if src.is_empty():
		src = ProjectSettings.globalize_path("res://").path_join(
			"../gdextension/meridian/test/fixtures/chunkpack/meridian/core/chunks/zone01").simplify_path()
	if not FileAccess.file_exists("%s/zone01.chunks.json" % src):
		_check("zone fixture dir resolved (%s)" % src, false)
		_finish()
		return
	_check("staged fixture pack into user://", _stage_pack(src, STAGE_GOOD))

	print("\n-- Zone MOUNTED: remotes snap to the heightfield --")
	await _verify_snapped()

	print("\n-- Flat bootstrap (NO zone): snap is a strict no-op --")
	await _verify_flat_noop()

	_finish()


func _verify_snapped() -> void:
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		return
	var world := packed.instantiate()
	world.configure({"zone": {"dir": STAGE_GOOD, "id": "zone01", "spawn": SPAWN}}, {"name": "Verifier"})
	root.add_child(world)
	await _wait(3)

	_check("zone is active (streamed terrain)", world.is_zone_active())
	_check("mover on the heightfield backend", world._mover != null and world._mover.is_heightfield_active())

	var guard := 0
	while world.is_zone_loading() and guard < 900:
		await physics_frame
		guard += 1
	_check("world revealed (spawn chunks resident)", not world.is_zone_loading())

	# ---- 1. THE BUG: a remote arrives at wire height 0 over ~23 m of terrain. ----
	var analytic := _fixture_height(SPAWN.x, SPAWN.z)
	var g: Dictionary = world._mover.sample_ground(SPAWN.x, SPAWN.z)
	_check("spawn XZ has real terrain under it (~%.2f m, walkable)" % analytic,
		bool(g.get("walkable", false)) and abs(float(g.get("height", 0.0)) - analytic) < 0.25)

	var node := _add_remote(world, 9001, Vector3(SPAWN.x, 0.0, SPAWN.z))
	_check("remote node spawned", node != null)
	if node == null:
		world.queue_free()
		return
	# The interpolator must actually have the snapshot, else the position write is
	# skipped and any height assertion below would pass/fail for the wrong reason.
	var s: Dictionary = world._interp.sample_entity(9001, world._client_ms)
	_check("interpolator has the remote buffered (sample kind != empty)",
		int(s.get("kind", KIND_EMPTY)) != KIND_EMPTY)
	_check("wire height really is 0 (server ground is flat — the premise of #885)",
		abs(float(s.get("y", -1.0))) < 0.001)

	world._update_remotes()
	_check("remote STANDS ON TERRAIN (y ~= %.2f, not buried at 0)" % analytic,
		abs(node.position.y - analytic) < 0.25)
	_check("remote is NOT at the raw wire height", absf(node.position.y) > 1.0)
	_check("remote x/z untouched by the snap (interpolation preserved)",
		is_equal_approx(node.position.x, SPAWN.x) and is_equal_approx(node.position.z, SPAWN.z))

	# ---- 2. A second remote on DIFFERENT ground gets THAT ground's height. ----
	# Proves the height is sampled per-entity at its own XZ, not a constant.
	var other := Vector3(-300.0, 0.0, -340.0)   # same resident cell (0,0), different height
	var oh := _fixture_height(other.x, other.z)
	_check("the two XZs really have different terrain heights (%.2f vs %.2f)" % [analytic, oh],
		absf(oh - analytic) > 0.5)
	var node2 := _add_remote(world, 9002, Vector3(other.x, 0.0, other.z))
	world._update_remotes()
	_check("second remote snaps to ITS OWN ground height (~%.2f)" % oh,
		node2 != null and abs(node2.position.y - oh) < 0.25)

	# ---- 3. Ground with no meaningful height must HOLD the wire height. ----
	# Cell (1,0)'s chunk is streamed around the spawn; pick an XZ far outside the
	# 3x3 grid so nothing is resident -> sample_ground reports NOT walkable. The
	# remote must keep its wire height, never get slammed to 0.
	var away := Vector3(3000.0, 0.0, 3000.0)
	_check("far XZ has no resident ground (not walkable)", not world._mover.has_ground_at(away.x, away.z))
	var node3 := _add_remote(world, 9003, Vector3(away.x, 42.0, away.z))
	world._update_remotes()
	_check("remote over non-walkable ground HOLDS its wire height (42, not 0)",
		node3 != null and abs(node3.position.y - 42.0) < 0.001)

	# ---- 4. Repeated updates are stable (no drift/jitter frame to frame). ----
	var y_first := node.position.y
	for _i in range(30):
		world._update_remotes()
		await physics_frame
	_check("height is stable across 30 frames (no jitter/drift)",
		absf(node.position.y - y_first) < 0.001)

	world.queue_free()
	await _wait(2)


func _verify_flat_noop() -> void:
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	var world := packed.instantiate()
	world.configure({}, {"name": "Verifier"})   # no zone -> D-19 flat bootstrap
	root.add_child(world)
	await _wait(3)

	_check("zone NOT active (flat bootstrap fallback)", not world.is_zone_active())
	_check("mover NOT on the heightfield backend",
		world._mover != null and not world._mover.is_heightfield_active())

	# A non-zero wire height is the discriminating case: if the snap ran here it
	# would resolve the flat plane (0) and drag the entity down to 0. It must not.
	var node := _add_remote(world, 9101, Vector3(12.0, 3.5, -7.0))
	world._update_remotes()
	_check("remote renders at its WIRE height, unchanged (3.5)",
		node != null and abs(node.position.y - 3.5) < 0.001)
	_check("remote x/z unchanged in the fallback",
		node != null and is_equal_approx(node.position.x, 12.0) and is_equal_approx(node.position.z, -7.0))

	# The ordinary flat-bootstrap case (wire height 0) is likewise untouched.
	var node2 := _add_remote(world, 9102, Vector3(4.0, 0.0, 4.0))
	world._update_remotes()
	_check("wire height 0 stays 0 in the fallback (behaviour unchanged)",
		node2 != null and abs(node2.position.y) < 0.001)

	world.queue_free()
	await _wait(2)


func _finish() -> void:
	print("\n%d checks, %d failures" % [_checks, _fails])
	# Completion sentinel: a run that aborts inside an await (runtime error) never
	# reaches this line, so "no sentinel" == "did not finish" regardless of exit code.
	print("[snap] MERIDIAN_REMOTE_GROUND_SNAP_VERIFY_COMPLETE checks=%d fails=%d" % [_checks, _fails])
	if _fails == 0:
		print("[snap] MERIDIAN_REMOTE_GROUND_SNAP_VERIFY_OK remotes_stand_on_terrain flat_noop")
		print("remote ground snap verify: ALL PASS")
	else:
		print("remote ground snap verify: %d FAILURE(S)" % _fails)
	quit(1 if _fails > 0 else 0)
