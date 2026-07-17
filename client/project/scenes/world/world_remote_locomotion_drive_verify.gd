# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the REMOTE-ENTITY locomotion
# DRIVE (story #909, W1c of the character-animation epic #906). NOT a shipped scene:
# a SceneTree script run
#   godot --headless --path client/project --editor --quit            # once, to import + seed the class cache
#   godot --headless --path client/project --script res://scenes/world/world_remote_locomotion_drive_verify.gd
# (same convention as world_local_locomotion_drive_verify.gd, #908). Proves, with no
# render and no server, that the SAME driver world.gd:_update_remotes uses
# (MeridianRemoteLocomotionDriver) maps a remote's INTERPOLATED planar motion —
# derived by differencing successive interpolated positions, NOT read off the wire
# (EntityUpdate carries only x/y/z/orientation, world.fbs:338-347) — into
# set_locomotion, driving the LIVE AnimationTree of a real AssembledCharacter to the
# matching state and tracking the derived planar speed on the walk↔run blend.
#
# WHY DERIVED, NOT DECODED: remotes have NO MoveMode/state_flags on the wire (only the
# LOCAL player's mover produces those, #908). So this driver differences the smoothed
# interpolated position each tick to recover a planar speed, thresholds a small idle
# deadband so interpolation noise on a standing remote reads as 'idle' (never a twitchy
# walk), and EMA-smooths the speed so the walk↔run blend does not jitter. grounded is
# ALWAYS true for remotes (no jump on the wire — that is the optional Wopt-wire
# follow-up), so the driver only ever selects 'idle' or 'ground', never 'air'.
#
# ⛔ CHECK-COUNT GUARD: a client verify that fails to LOAD (missing framework / stale
# class cache) can still exit 0 with zero assertions — a false green. This script
# counts every check and FAILS if fewer than MIN_CHECKS ran, and fails on any failed
# assertion. NEVER trust the exit code alone; read the PASS/FAIL lines.
#
# Exits 0 only when every check passed AND at least MIN_CHECKS checks ran.

extends SceneTree

const AssembledCharacterScript := preload("res://characters/assembled_character.gd")
const LocomotionScript := preload("res://characters/locomotion.gd")
const RemoteDriverScript := preload("res://characters/remote_locomotion_driver.gd")

# The blend-space anchor speeds (m/s) the ground blend is keyed to
# (locomotion.gd _WALK_SPEED / _RUN_SPEED) — the derived speed must land near these.
const WALK_SPEED: float = 2.5
const RUN_SPEED: float = 6.0

# The remote sim tick the interpolator is sampled at (world.gd TICK_MS = 50ms). The
# driver differences positions over exactly this dt, so the verify advances the fake
# remote by speed * DT each observe — the SAME cadence world.gd:_update_remotes uses.
const DT: float = 0.05

# Ticks to feed a constant-velocity remote so the EMA converges tight to the raw speed
# before asserting the blend (0.65^40 ≈ 4e-8 residual — effectively exact).
const CONVERGE_TICKS: int = 40

# Floor on the number of checks that MUST run — a hard guard against a load failure
# silently reducing the suite to zero assertions (false green).
const MIN_CHECKS: int = 16

var _fails: int = 0
var _ran: int = 0


func _check(name: String, ok: bool) -> void:
	_ran += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	_run()


func _run() -> void:
	print("meridian REMOTE-ENTITY locomotion DRIVE verify (#909)")

	# --- pure classification (the speed → mode decode) ------------------------
	_verify_mode_for_speed()

	# --- the drive: derived interpolated speed → real AssembledCharacter tree --
	var ac = AssembledCharacterScript.new()
	ac.name = "remote_body_under_test"
	root.add_child(ac)
	await process_frame
	var ok: bool = ac.assemble(1, 0, {"hair": 1, "face": 1, "skin": 1}, [])
	_check("assemble returns true (remote body has a skeleton + tree)",
		ok and ac.is_assembled())
	await process_frame   # the AnimationMixer builds its bone-track caches
	await process_frame

	var tree: AnimationTree = ac.locomotion_tree()
	_check("remote body has a locomotion AnimationTree", tree != null)
	if tree == null:
		_finish()
		return
	var playback: AnimationNodeStateMachinePlayback = tree.get("parameters/playback")
	_check("state-machine playback is reachable", playback != null)
	if playback == null:
		_finish()
		return

	_verify_drive_idle(ac, tree, playback)
	_verify_drive_jitter_is_idle(ac, tree, playback)
	_verify_drive_walk(ac, tree, playback)
	_verify_drive_run(ac, tree, playback)
	_verify_grounded_never_air(ac, tree, playback)
	_verify_capsule_noop()

	root.remove_child(ac)
	ac.free()
	_finish()


func _finish() -> void:
	var enough: bool = _ran >= MIN_CHECKS
	print("  [%s] check-count guard — %d checks ran (>= %d required)"
		% ["PASS" if enough else "FAIL", _ran, MIN_CHECKS])
	if not enough:
		_fails += 1
	print("\n%s" % ("ALL REMOTE-DRIVE CHECKS PASS (%d checks)" % _ran if _fails == 0
		else "%d REMOTE-DRIVE FAILURE(S) of %d checks" % [_fails, _ran]))
	quit(0 if _fails == 0 else 1)


# Step the AnimationTree deterministically so travel()s settle.
func _settle(tree: AnimationTree, dt: float, steps: int) -> void:
	for _i in range(steps):
		tree.advance(dt)


# Advance a fresh driver at a constant planar velocity along +X for `ticks`, feeding
# the SAME (body, position, DT) call world.gd:_update_remotes makes, then settle the
# tree. Returns the driver so callers can read its last derived speed.
func _drive_constant(driver, ac, speed: float, ticks: int, tree: AnimationTree) -> void:
	var pos := Vector3.ZERO
	for _i in range(ticks):
		pos += Vector3(speed * DT, 0.0, 0.0)   # advance planar by speed*dt each tick
		driver.drive(ac, pos, DT)
	_settle(tree, 0.2, 4)


# --- A. speed → mode classification (idle deadband, walk/run split) -----------
func _verify_mode_for_speed() -> void:
	print(" MeridianRemoteLocomotionDriver.mode_for_speed classifies derived speed:")
	_check("0 m/s → Idle (dead stop)",
		RemoteDriverScript.mode_for_speed(0.0) == RemoteDriverScript.MODE_IDLE)
	# Interpolation noise below the deadband must read as idle, never a twitchy walk.
	_check("sub-deadband jitter → Idle (no twitch)",
		RemoteDriverScript.mode_for_speed(RemoteDriverScript.IDLE_SPEED_EPSILON * 0.5)
			== RemoteDriverScript.MODE_IDLE)
	_check("walk speed (2.5) → Walk",
		RemoteDriverScript.mode_for_speed(WALK_SPEED) == RemoteDriverScript.MODE_WALK)
	_check("run speed (6.0) → Run",
		RemoteDriverScript.mode_for_speed(RUN_SPEED) == RemoteDriverScript.MODE_RUN)


# --- B. idle: a stationary remote (position never changes) → 'idle' -----------
func _verify_drive_idle(ac, tree: AnimationTree, playback) -> void:
	print(" drive(stationary remote) → live state 'idle':")
	var driver = RemoteDriverScript.new()
	var pos := Vector3(7.0, 0.0, -3.0)
	for _i in range(CONVERGE_TICKS):
		driver.drive(ac, pos, DT)   # same position every tick → zero derived speed
	_settle(tree, 0.2, 4)
	_check("derived speed of a still remote is ~0",
		driver.smoothed_speed() < RemoteDriverScript.IDLE_SPEED_EPSILON)
	_check("stationary remote → AnimationTree in 'idle'",
		String(playback.get_current_node()) == LocomotionScript.STATE_IDLE)


# --- C. sub-threshold jitter reads as idle, not a twitchy walk ----------------
# Feed a tiny alternating position wobble (interpolation noise) whose per-tick planar
# delta stays under the deadband: the tree must hold 'idle', never flip to 'ground'.
func _verify_drive_jitter_is_idle(ac, tree: AnimationTree, playback) -> void:
	print(" drive(noisy-but-still remote) → stays 'idle' (no jitter twitch):")
	var driver = RemoteDriverScript.new()
	var base := Vector3(2.0, 0.0, 2.0)
	# Wobble amplitude chosen so raw per-tick speed (2*amp/DT) stays under the deadband.
	var amp: float = (RemoteDriverScript.IDLE_SPEED_EPSILON * DT) * 0.25
	for i in range(CONVERGE_TICKS):
		var jitter := amp if (i % 2 == 0) else -amp
		driver.drive(ac, base + Vector3(jitter, 0.0, 0.0), DT)
	_settle(tree, 0.2, 4)
	_check("jittering-in-place remote → AnimationTree stays 'idle'",
		String(playback.get_current_node()) == LocomotionScript.STATE_IDLE)


# --- D. walk: remote advancing at walk speed → 'ground', blend ~ walk speed ---
func _verify_drive_walk(ac, tree: AnimationTree, playback) -> void:
	print(" drive(remote advancing at walk speed) → live 'ground', blend ~ walk:")
	var driver = RemoteDriverScript.new()
	_drive_constant(driver, ac, WALK_SPEED, CONVERGE_TICKS, tree)
	_check("walk-speed remote → derived speed converges to ~2.5 m/s",
		absf(driver.smoothed_speed() - WALK_SPEED) < 0.1)
	_check("walk-speed remote → AnimationTree in 'ground'",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)
	var blend: float = float(tree.get("parameters/%s/blend_position"
		% LocomotionScript.STATE_GROUND))
	# Blend lands in the walk band (nearer the 2.5 anchor than the 6.0 run anchor).
	_check("ground blend_position tracks walk speed (in [2.0, 3.0])",
		blend > 2.0 and blend < 3.0)


# --- E. run: remote advancing at run speed → 'ground', blend ~ run speed ------
func _verify_drive_run(ac, tree: AnimationTree, playback) -> void:
	print(" drive(remote advancing at run speed) → live 'ground', blend ~ run:")
	var driver = RemoteDriverScript.new()
	_drive_constant(driver, ac, RUN_SPEED, CONVERGE_TICKS, tree)
	_check("run-speed remote → derived speed converges to ~6.0 m/s",
		absf(driver.smoothed_speed() - RUN_SPEED) < 0.1)
	_check("run-speed remote → AnimationTree in 'ground'",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)
	var blend: float = float(tree.get("parameters/%s/blend_position"
		% LocomotionScript.STATE_GROUND))
	_check("ground blend_position tracks run speed (>= 5.5)", blend >= 5.5)


# --- F. remotes are ALWAYS grounded → never 'air' -----------------------------
# No jump on the wire for remotes; even a large vertical jump between samples must not
# select 'air' (the vertical component is excluded from the planar-speed derivation and
# grounded is hard-true). A fast planar+vertical move → 'ground', never 'air'.
func _verify_grounded_never_air(ac, tree: AnimationTree, playback) -> void:
	print(" drive(remote with vertical motion) → 'ground'/'idle', NEVER 'air':")
	var driver = RemoteDriverScript.new()
	var pos := Vector3.ZERO
	for _i in range(CONVERGE_TICKS):
		# Big vertical component alongside a run-speed planar advance.
		pos += Vector3(RUN_SPEED * DT, 5.0 * DT, 0.0)
		driver.drive(ac, pos, DT)
	_settle(tree, 0.2, 4)
	_check("vertical motion does NOT inflate the derived planar speed",
		absf(driver.smoothed_speed() - RUN_SPEED) < 0.1)
	_check("remote with vertical motion → 'ground' (never 'air')",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)


# --- G. capsule fallback (no set_locomotion) → drive is a safe no-op ----------
# A remote whose appearance was absent / assembly failed renders the class-colored
# capsule (a plain MeshInstance3D). drive() must silently skip it, never crash.
func _verify_capsule_noop() -> void:
	print(" drive on a capsule fallback (no set_locomotion) is a safe no-op:")
	var capsule := MeshInstance3D.new()   # the fallback body type _build_capsule_body makes
	var driver = RemoteDriverScript.new()
	driver.drive(capsule, Vector3(1, 0, 0), DT)   # must not crash
	driver.drive(capsule, Vector3(2, 0, 0), DT)
	driver.drive(null, Vector3(3, 0, 0), DT)       # null body must not crash either
	_check("drive on a non-AssembledCharacter body is a safe no-op (no crash)", true)
	capsule.free()
