# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the JUMP INPUT wiring
# (story #905). NOT a shipped scene: a SceneTree script run
#   godot --headless --path client/project --import   # once, to seed the class cache
#   godot --headless --path client/project --script res://scenes/world/jump_input_verify.gd
# (same convention as input_move_verify.gd / world_local_locomotion_drive_verify.gd).
#
# The gap #905 closed: the movement physics, prediction and wire protocol already
# carried jump end to end (MeridianMovementController.predict's `jump` arg →
# integrate_tick arc → state_flags bit 7 kJump → server validation), but the client
# never pressed it — _tick_local_player hard-coded jump=false. This proves, with no
# render and no server, the REAL wiring:
#   * MovementBasis.grounded_jump (the SAME gate world.gd:_tick_local_player calls)
#     honours a jump press ONLY while grounded — no air-jump / double-jump, and no
#     spurious kJump intent emitted mid-air;
#   * feeding that gated value into a REAL MeridianMovementController launches the
#     arc off the ground and stamps kJump (bit 7) on the jump tick; and
#   * the resulting airborne state drives a REAL AssembledCharacter's live
#     AnimationTree into the 'air' state — the W1b (#908) locomotion-driver link
#     that is the visible payoff of pressing space.
#
# ⛔ CHECK-COUNT GUARD: a client verify that fails to LOAD (missing framework /
# stale class cache) can still exit 0 with zero assertions — a false green. This
# script counts every check and FAILS if fewer than MIN_CHECKS ran, and fails on
# any failed assertion. NEVER trust the exit code alone; read the PASS/FAIL lines.
#
# Exits 0 only when every check passed AND at least MIN_CHECKS checks ran.

extends SceneTree

const MovementBasis := preload("res://scenes/world/movement_basis.gd")
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")
const LocomotionScript := preload("res://characters/locomotion.gd")
const LocomotionDriverScript := preload("res://characters/locomotion_driver.gd")

# state_flags bit 7 = kJump ("jump requested this tick", movement_constants.h §3).
const KJUMP_BIT: int = 1 << 7   # 0x80

# Floor on the number of checks that MUST run — a hard guard against a load failure
# silently reducing the suite to zero assertions (false green).
const MIN_CHECKS: int = 14

var _fails: int = 0
var _ran: int = 0


func _check(name: String, ok: bool) -> void:
	_ran += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	_run()


# True iff the 'jump' action carries a keyboard event bound to KEY_SPACE (by
# physical keycode — the layout-independent bind the project.godot entry uses).
func _jump_bound_to_space() -> bool:
	if not InputMap.has_action("jump"):
		return false
	for ev in InputMap.action_get_events("jump"):
		if ev is InputEventKey and (ev as InputEventKey).physical_keycode == KEY_SPACE:
			return true
	return false


func _run() -> void:
	print("meridian JUMP INPUT wiring verify (#905)")

	var mover_ok := ClassDB.class_exists("MeridianMovementController")
	_check("MeridianMovementController class registered", mover_ok)
	if not mover_ok:
		_finish()
		return

	# The project.godot [input] addition must actually load as an InputMap action —
	# this is what world.gd:_tick_local_player reads via Input.is_action_pressed.
	_check("project.godot defines the 'jump' input action",
		InputMap.has_action("jump"))
	_check("'jump' action is bound to a physical key event (spacebar)",
		_jump_bound_to_space())

	_verify_gate()
	await _verify_grounded_launch()
	await _verify_air_animation_payoff()
	_finish()


# --- A. the pure gate: jump is honoured ONLY when grounded --------------------
# grounded_jump is the exact expression world.gd:_tick_local_player evaluates to
# decide the `jump` arg it feeds predict(); assert every truth-table row.
func _verify_gate() -> void:
	print(" MovementBasis.grounded_jump gates the raw press on grounded:")
	_check("grounded + pressed  → jump  (launch this tick)",
		MovementBasis.grounded_jump(true, true) == true)
	_check("grounded + released → no jump",
		MovementBasis.grounded_jump(false, true) == false)
	_check("airborne + pressed  → no jump (no air-jump / double-jump)",
		MovementBasis.grounded_jump(true, false) == false)
	_check("airborne + released → no jump",
		MovementBasis.grounded_jump(false, false) == false)


# --- B. real mover: gated press launches the arc + stamps kJump; a mid-air ----
#        press does NOT re-launch and emits NO kJump (the gate is load-bearing).
func _verify_grounded_launch() -> void:
	print(" a grounded, gated press launches a REAL mover and stamps kJump:")
	var mover := MeridianMovementController.new()   # seeded grounded at origin (D-19 flat map)
	_check("fresh mover reports grounded at spawn", mover.is_grounded())

	# The grounded tick: the gate yields true, exactly as world.gd would compute it.
	var jump := MovementBasis.grounded_jump(true, mover.is_grounded())
	_check("gate yields jump=true while grounded", jump)
	var intent: Dictionary = mover.predict(Vector3.ZERO, false, jump, 0.0, 0)
	_check("jump tick stamps kJump (state_flags bit 7 set)",
		(int(intent.get("state_flags", 0)) & KJUMP_BIT) != 0)
	_check("mover leaves the ground after the launch tick",
		not mover.is_grounded())
	_check("predicted body rose above the ground (y > 0)",
		mover.get_predicted_position().y > 0.0)

	# Mid-air, holding the same key: the gate now yields FALSE, so no kJump intent is
	# emitted and no second impulse is applied — no double-jump.
	var vy_before: float = mover.get_predicted_velocity().y
	var jump_air := MovementBasis.grounded_jump(true, mover.is_grounded())
	_check("gate yields jump=false while airborne (holding the key)", not jump_air)
	var intent2: Dictionary = mover.predict(Vector3.ZERO, false, jump_air, 0.0, 20)
	_check("no kJump emitted on the airborne tick (state_flags bit 7 clear)",
		(int(intent2.get("state_flags", 0)) & KJUMP_BIT) == 0)
	_check("no re-launch mid-air: vertical velocity keeps falling under gravity",
		mover.get_predicted_velocity().y < vy_before)

	# The gate is load-bearing: WITHOUT it, feeding the raw press mid-air would stamp
	# a spurious kJump intent (the core still refuses the physical re-launch, but the
	# server would see a bogus jump-requested flag). Prove that contrast on a fresh
	# airborne mover so the gate's value is unambiguous.
	var m2 := MeridianMovementController.new()
	m2.predict(Vector3.ZERO, false, true, 0.0, 0)    # launch off the ground
	var ungated: Dictionary = m2.predict(Vector3.ZERO, false, true, 0.0, 20)  # raw press, airborne
	_check("UNgated raw press mid-air WOULD stamp kJump — why the gate exists",
		(int(ungated.get("state_flags", 0)) & KJUMP_BIT) != 0)
	_check("core still refuses the physical double-jump even ungated (stays airborne)",
		not m2.is_grounded())


# --- C. the visible payoff: the airborne state drives the AnimationTree to 'air'
# The W1b (#908) locomotion driver reads is_grounded()/mode every tick, so the jump
# the input now triggers flows straight into the air animation. Drive a REAL
# AssembledCharacter tree from a jump-launched mover and assert the live state.
func _verify_air_animation_payoff() -> void:
	print(" the jump-launched airborne state drives the live AnimationTree to 'air':")
	var ac = AssembledCharacterScript.new()
	ac.name = "jump_input_body_under_test"
	root.add_child(ac)
	await process_frame
	var assembled: bool = ac.assemble(1, 0, {"hair": 1, "face": 1, "skin": 1}, [])
	_check("assemble returns true (local body has a skeleton + tree)",
		assembled and ac.is_assembled())
	await process_frame   # the AnimationMixer builds its bone-track caches
	await process_frame

	var tree: AnimationTree = ac.locomotion_tree()
	_check("local body has a locomotion AnimationTree", tree != null)
	if tree == null:
		root.remove_child(ac)
		ac.free()
		return
	var playback: AnimationNodeStateMachinePlayback = tree.get("parameters/playback")
	_check("state-machine playback is reachable", playback != null)
	if playback == null:
		root.remove_child(ac)
		ac.free()
		return

	# Launch a real mover with the SAME gated press, then keep it airborne one tick,
	# exactly the sequence _tick_local_player produces on a jump.
	var mover := MeridianMovementController.new()
	var jump := MovementBasis.grounded_jump(true, mover.is_grounded())
	mover.predict(Vector3.ZERO, false, jump, 0.0, 0)           # launch (grounded → false)
	var intent: Dictionary = mover.predict(Vector3.ZERO, false,
		MovementBasis.grounded_jump(true, mover.is_grounded()), 0.0, 20)   # still airborne
	_check("mover is airborne on the tick fed to the driver", not mover.is_grounded())
	LocomotionDriverScript.drive(ac, mover, int(intent.get("state_flags", 0)))
	for _i in range(4):
		tree.advance(0.2)
	_check("pressing jump drives the AnimationTree into 'air' (the W1b payoff)",
		String(playback.get_current_node()) == LocomotionScript.STATE_AIR)

	root.remove_child(ac)
	ac.free()


func _finish() -> void:
	var enough: bool = _ran >= MIN_CHECKS
	print("  [%s] check-count guard — %d checks ran (>= %d required)"
		% ["PASS" if enough else "FAIL", _ran, MIN_CHECKS])
	if not enough:
		_fails += 1
	print("\n%s" % ("ALL JUMP-INPUT CHECKS PASS (%d checks)" % _ran if _fails == 0
		else "%d JUMP-INPUT FAILURE(S) of %d checks" % [_fails, _ran]))
	quit(0 if _fails == 0 else 1)
