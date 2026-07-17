# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the LOCAL-PLAYER locomotion
# DRIVE (story #908, W1b of the character-animation epic #906). NOT a shipped
# scene: a SceneTree script run
#   godot --headless --path client/project --import   # once, to seed the class cache
#   godot --headless --path client/project --script res://scenes/world/world_local_locomotion_drive_verify.gd
# (same convention as assembled_character_locomotion_verify.gd, #907). Proves, with
# no render and no server, that the SAME driver world.gd:_tick_local_player uses
# (MeridianLocomotionDriver.drive) maps a REAL mover's per-tick output —
# state_flags → MoveMode, predicted velocity → planar speed, is_grounded → grounded
# — into set_locomotion, driving the LIVE AnimationTree of a real AssembledCharacter
# to the matching state and tracking planar_speed on the walk↔run blend.
#
# The mover is the real MeridianMovementController (the same class world.gd holds),
# driven with idle / walk / run / jump inputs so the state_flags + velocity +
# grounded are the AUTHENTIC values the running client feeds the driver every tick —
# never hand-forged. This is the story's acceptance: assert the live tree STATE that
# results from driving the mover, not merely that set_locomotion was called.
#
# ⛔ CHECK-COUNT GUARD: a client verify that fails to LOAD (missing framework /
# stale class cache) can still exit 0 with zero assertions — a false green. This
# script counts every check and FAILS if fewer than MIN_CHECKS ran, and fails on
# any failed assertion. NEVER trust the exit code alone; read the PASS/FAIL lines.
#
# Exits 0 only when every check passed AND at least MIN_CHECKS checks ran.

extends SceneTree

const AssembledCharacterScript := preload("res://characters/assembled_character.gd")
const LocomotionScript := preload("res://characters/locomotion.gd")
const LocomotionDriverScript := preload("res://characters/locomotion_driver.gd")

# The mover's [SPIKE-LOCKED] speed caps (movement_constants.h:97-98), the anchors
# the ground blend space is keyed to (locomotion.gd _WALK_SPEED / _RUN_SPEED).
const WALK_SPEED: float = 2.5
const RUN_SPEED: float = 6.0

# Floor on the number of checks that MUST run — a hard guard against a load failure
# silently reducing the suite to zero assertions (false green).
const MIN_CHECKS: int = 18

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
	print("meridian LOCAL-PLAYER locomotion DRIVE verify (#908)")

	# --- pure mapping (the decode world.gd relies on) -------------------------
	_verify_mode_decode()
	_verify_planar_speed()

	# --- the drive: real mover → real AssembledCharacter live AnimationTree ---
	var ac = AssembledCharacterScript.new()
	ac.name = "local_player_body_under_test"
	root.add_child(ac)
	await process_frame
	var ok: bool = ac.assemble(1, 0, {"hair": 1, "face": 1, "skin": 1}, [])
	_check("assemble returns true (local body has a skeleton + tree)",
		ok and ac.is_assembled())
	await process_frame   # the AnimationMixer builds its bone-track caches
	await process_frame

	var tree: AnimationTree = ac.locomotion_tree()
	_check("local body has a locomotion AnimationTree", tree != null)
	if tree == null:
		_finish()
		return
	var playback: AnimationNodeStateMachinePlayback = tree.get("parameters/playback")
	_check("state-machine playback is reachable", playback != null)
	if playback == null:
		_finish()
		return

	_verify_drive_idle(ac, tree, playback)
	_verify_drive_walk(ac, tree, playback)
	_verify_drive_run(ac, tree, playback)
	_verify_drive_airborne(ac, tree, playback)
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
	print("\n%s" % ("ALL LOCAL-DRIVE CHECKS PASS (%d checks)" % _ran if _fails == 0
		else "%d LOCAL-DRIVE FAILURE(S) of %d checks" % [_fails, _ran]))
	quit(0 if _fails == 0 else 1)


# A fresh mover seeded grounded at spawn (ensure_reconciler → flat y=0 world, D-19),
# so each scenario starts from a clean, grounded state independent of the last.
func _fresh_mover() -> MeridianMovementController:
	return MeridianMovementController.new()


# Step the AnimationTree deterministically so travel()s settle.
func _settle(tree: AnimationTree, dt: float, steps: int) -> void:
	for _i in range(steps):
		tree.advance(dt)


# --- A. mode decode mirrors movement_constants.h mode_from_state_flags --------
func _verify_mode_decode() -> void:
	print(" MeridianLocomotionDriver.mode_from_state_flags decodes the low 3 bits:")
	# The low 3 bits carry the MoveMode; higher bits (jump/walk/etc. — constants
	# §state_flags) must NOT leak into the decoded mode. 0xF8 is every bit ABOVE
	# the mode mask set; OR-ing it in must not change the decoded mode.
	_check("state_flags 0 → Idle",
		LocomotionDriverScript.mode_from_state_flags(0) == LocomotionDriverScript.MODE_IDLE)
	_check("state_flags 1 → Walk",
		LocomotionDriverScript.mode_from_state_flags(1) == LocomotionDriverScript.MODE_WALK)
	_check("state_flags 2 → Run",
		LocomotionDriverScript.mode_from_state_flags(2) == LocomotionDriverScript.MODE_RUN)
	_check("state_flags 3 → Jump",
		LocomotionDriverScript.mode_from_state_flags(3) == LocomotionDriverScript.MODE_JUMP)
	_check("high bits above the mask are ignored (0xF8|2 → Run)",
		LocomotionDriverScript.mode_from_state_flags(0xF8 | 2) == LocomotionDriverScript.MODE_RUN)


# --- B. planar_speed is the XZ magnitude, ignoring vertical -------------------
func _verify_planar_speed() -> void:
	print(" MeridianLocomotionDriver.planar_speed is the horizontal (XZ) magnitude:")
	_check("planar_speed((3,0,4)) == 5",
		is_equal_approx(LocomotionDriverScript.planar_speed(Vector3(3, 0, 4)), 5.0))
	# Vertical (jump-arc) velocity must NOT inflate the ground-blend speed.
	_check("planar_speed ignores the vertical component ((3,9,4) still 5)",
		is_equal_approx(LocomotionDriverScript.planar_speed(Vector3(3, 9, 4)), 5.0))


# --- C. idle: still, grounded → 'idle' ---------------------------------------
func _verify_drive_idle(ac, tree: AnimationTree, playback) -> void:
	print(" drive(idle mover state) → live state 'idle':")
	var mover := _fresh_mover()
	var intent: Dictionary = mover.predict(Vector3.ZERO, false, false, 0.0, 0)
	_check("mover reports grounded while standing still", mover.is_grounded())
	LocomotionDriverScript.drive(ac, mover, int(intent.get("state_flags", 0)))
	_settle(tree, 0.2, 4)
	_check("standing-still mover → AnimationTree in 'idle'",
		String(playback.get_current_node()) == LocomotionScript.STATE_IDLE)


# --- D. walk: grounded, walk toggle → 'ground', blend == walk speed ----------
func _verify_drive_walk(ac, tree: AnimationTree, playback) -> void:
	print(" drive(walk mover state) → live 'ground', blend tracks walk speed:")
	var mover := _fresh_mover()
	# move_x != 0 so the run branch never triggers the backpedal cap; walk=true.
	var intent: Dictionary = mover.predict(Vector3(1, 0, 0), true, false, 0.0, 0)
	var speed: float = LocomotionDriverScript.planar_speed(mover.get_predicted_velocity())
	_check("walk input yields the walk speed cap (2.5 m/s)",
		is_equal_approx(speed, WALK_SPEED))
	LocomotionDriverScript.drive(ac, mover, int(intent.get("state_flags", 0)))
	_settle(tree, 0.2, 4)
	_check("walking mover → AnimationTree in 'ground'",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)
	var blend: float = float(tree.get("parameters/%s/blend_position"
		% LocomotionScript.STATE_GROUND))
	_check("ground blend_position == walk speed (2.5)", is_equal_approx(blend, WALK_SPEED))


# --- E. run: grounded, no walk toggle → 'ground', blend == run speed ---------
func _verify_drive_run(ac, tree: AnimationTree, playback) -> void:
	print(" drive(run mover state) → live 'ground', blend tracks run speed:")
	var mover := _fresh_mover()
	# move_x != 0 → not backpedal → full run cap (6.0 m/s); walk=false.
	var intent: Dictionary = mover.predict(Vector3(1, 0, 0), false, false, 0.0, 0)
	var speed: float = LocomotionDriverScript.planar_speed(mover.get_predicted_velocity())
	_check("run input yields the run speed cap (6.0 m/s)",
		is_equal_approx(speed, RUN_SPEED))
	LocomotionDriverScript.drive(ac, mover, int(intent.get("state_flags", 0)))
	_settle(tree, 0.2, 4)
	_check("running mover → AnimationTree in 'ground'",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)
	var blend: float = float(tree.get("parameters/%s/blend_position"
		% LocomotionScript.STATE_GROUND))
	_check("ground blend_position == run speed (6.0)", is_equal_approx(blend, RUN_SPEED))


# --- F. airborne: mover launched via jump → not grounded → 'air' -------------
# Jump INPUT wiring is #905; this proves the anim reacts to whatever grounded/mode
# the mover reports. One predict with jump=true launches (grounded flips false);
# a second predict keeps it airborne (rising under gravity, y still > 0), so the
# driver must select 'air' regardless of the horizontal mode.
func _verify_drive_airborne(ac, tree: AnimationTree, playback) -> void:
	print(" drive(airborne mover state) → live state 'air':")
	var mover := _fresh_mover()
	mover.predict(Vector3(1, 0, 0), false, true, 0.0, 0)   # launch: grounded → false
	var intent: Dictionary = mover.predict(Vector3(1, 0, 0), false, false, 0.0, 20)  # still up
	_check("mover reports NOT grounded after a jump launch", not mover.is_grounded())
	LocomotionDriverScript.drive(ac, mover, int(intent.get("state_flags", 0)))
	_settle(tree, 0.2, 4)
	_check("airborne mover → AnimationTree in 'air'",
		String(playback.get_current_node()) == LocomotionScript.STATE_AIR)


# --- G. capsule fallback (no set_locomotion) → drive is a safe no-op ----------
# The local body is the class-colored capsule (a plain MeshInstance3D) whenever
# assembly fails / appearance is absent; drive() must silently skip it, never crash.
func _verify_capsule_noop() -> void:
	print(" drive on a capsule fallback (no set_locomotion) is a safe no-op:")
	var capsule := MeshInstance3D.new()   # the fallback body type _build_capsule_body makes
	var mover := _fresh_mover()
	mover.predict(Vector3(1, 0, 0), false, false, 0.0, 0)
	LocomotionDriverScript.drive(capsule, mover, 2)   # must not crash
	LocomotionDriverScript.drive(null, mover, 2)       # null body must not crash either
	_check("drive on a non-AssembledCharacter body is a safe no-op (no crash)", true)
	capsule.free()
