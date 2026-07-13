# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for WASD-follows-facing
# (CHR-02, issue #619). NOT a shipped scene: run under
# `godot --headless --path project --script res://scenes/world/input_move_verify.gd`
# so CI / a dev box can PROVE, with no human at a mouse, that keyboard "forward"
# tracks the character's actual facing for every yaw — the exact defect #619 fixed.
#
# It drives the REAL pieces: a MeridianTpsCamera fed a synthetic RMB-steer turns
# a real Body node (yaw_target = ../Body), then the SHARED production basis
# (movement_basis.gd, the same code world.gd _tick_local_player calls) maps WASD
# into world space. The asserts compare that move vector against the Body node's
# ACTUAL transform basis — so "W = +facing, S = -facing, D = +right, A = -right"
# is measured against what the player literally sees, at several non-zero yaws.
# It also feeds the real MeridianMovementController to confirm the core still
# normalizes diagonals (W+D is not faster than W). Exits 0 on success, 1 on any
# failed assertion.

extends SceneTree

const MovementBasis := preload("res://scenes/world/movement_basis.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _approx(a: float, b: float, eps := 1e-3) -> bool:
	return abs(a - b) <= eps


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("meridian WASD-follows-facing RUNTIME verify (#619)")

	var loaded := ClassDB.class_exists("MeridianTpsCamera")
	_check("MeridianTpsCamera class registered", loaded)
	var mover_ok := ClassDB.class_exists("MeridianMovementController")
	_check("MeridianMovementController class registered", mover_ok)
	if not loaded or not mover_ok:
		quit(1)
		return

	# --- Minimal player rig: Player -> {Body, TpsCamera(yaw_target=../Body)} ----
	var world := Node3D.new()
	root.add_child(world)
	var player := Node3D.new()
	player.name = "Player"
	world.add_child(player)
	var body := Node3D.new()
	body.name = "Body"
	player.add_child(body)
	var cam := ClassDB.instantiate("MeridianTpsCamera") as Node3D
	cam.name = "TpsCamera"
	cam.set("capture_mouse", false)
	cam.set("yaw_target_path", NodePath("../Body"))
	player.add_child(cam)
	await _wait(2)

	# Drive several distinct facings by RMB-steering, and at EACH one prove the
	# emitted WASD vectors line up with the Body's real forward/right. A single
	# yaw could hide a sign error that only bites in one quadrant; sweeping both
	# directions (and past +/-90 deg) exercises all four.
	var steers: Array = [60.0, 90.0, -140.0, 120.0]
	for step_dx in steers:
		cam.feed_mouse_motion(step_dx, 0.0, 1)  # 1 = Steer (RMB)
		await _wait(1)
		var st: Dictionary = cam.get_state()
		var yaw: float = st["character_yaw"]

		# (a) steer updates character_yaw, and (b) the Body node is actually there.
		_check("steer updated character_yaw (yaw=%.3f)" % yaw, abs(yaw) > 1e-4)
		_check("Body.rotation.y tracks character_yaw @ yaw=%.3f" % yaw,
			_approx(body.rotation.y, yaw))

		# Ground-truth facing/right from the Body's REAL transform (what the player
		# sees). Godot forward = local -Z, right = local +X.
		var basis := body.global_transform.basis
		var facing := (-basis.z)
		facing.y = 0.0
		facing = facing.normalized()
		var right := basis.x
		right.y = 0.0
		right = right.normalized()

		# (c) the SHARED production basis maps WASD to world space; compare to facing.
		var mW := MovementBasis.character_relative_move(1.0, 0.0, yaw).normalized()
		var mS := MovementBasis.character_relative_move(-1.0, 0.0, yaw).normalized()
		var mD := MovementBasis.character_relative_move(0.0, 1.0, yaw).normalized()
		var mA := MovementBasis.character_relative_move(0.0, -1.0, yaw).normalized()
		_check("W drives ALONG the Body facing @ yaw=%.3f (dot=%.4f)" % [yaw, mW.dot(facing)],
			_approx(mW.dot(facing), 1.0, 1e-3))
		_check("S backpedals opposite the facing @ yaw=%.3f (dot=%.4f)" % [yaw, mS.dot(facing)],
			_approx(mS.dot(facing), -1.0, 1e-3))
		_check("D strafes to the Body RIGHT @ yaw=%.3f (dot=%.4f)" % [yaw, mD.dot(right)],
			_approx(mD.dot(right), 1.0, 1e-3))
		_check("A strafes to the Body LEFT @ yaw=%.3f (dot=%.4f)" % [yaw, mA.dot(right)],
			_approx(mA.dot(right), -1.0, 1e-3))

	# --- Diagonal speed: the controller core must normalize so W+D is not faster.
	# Feed the REAL MeridianMovementController the world-space move for W and for
	# W+D at a non-zero yaw and compare predicted horizontal speed.
	var mover := MeridianMovementController.new()
	mover.reset(Vector3.ZERO, 0.0)
	var yaw2: float = cam.get_state()["character_yaw"]
	var t := 0
	var moveW: Vector3 = MovementBasis.character_relative_move(1.0, 0.0, yaw2)
	mover.predict(moveW, false, false, yaw2, t)
	t += 50
	mover.predict(moveW, false, false, yaw2, t)
	var vW: Vector3 = mover.get_predicted_velocity()
	var spW := Vector2(vW.x, vW.z).length()

	var mover2 := MeridianMovementController.new()
	mover2.reset(Vector3.ZERO, 0.0)
	var moveWD: Vector3 = MovementBasis.character_relative_move(1.0, 1.0, yaw2)
	t = 0
	mover2.predict(moveWD, false, false, yaw2, t)
	t += 50
	mover2.predict(moveWD, false, false, yaw2, t)
	var vWD: Vector3 = mover2.get_predicted_velocity()
	var spWD := Vector2(vWD.x, vWD.z).length()
	print("  W speed=%.4f  W+D speed=%.4f" % [spW, spWD])
	_check("diagonal (W+D) is NOT faster than W (core normalizes)",
		_approx(spWD, spW, 1e-2) and spW > 0.0)

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)
