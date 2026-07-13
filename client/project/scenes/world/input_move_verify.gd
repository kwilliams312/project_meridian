# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for WASD-follows-the-VIEW
# (CHR-02). NOT a shipped scene: run under
# `godot --headless --path project --script res://scenes/world/input_move_verify.gd`
# so CI / a dev box can PROVE, with no human at a mouse, that keyboard "forward"
# tracks the camera's LOOK direction (camera_yaw) for every yaw — including the
# case where the view has been orbited AWAY from the character's committed facing.
#
# world.gd _tick_local_player uses `_camera.get_camera_yaw()` (the VIEW) and the
# shared basis (movement_basis.gd). So this drives the REAL MeridianTpsCamera:
#   * RMB-steer turns the view AND the character together (camera_yaw==character_yaw)
#     — W must drive along the view, which here equals the visible Body facing.
#   * LMB-orbit turns ONLY the view (camera_yaw != character_yaw) — W must follow
#     the VIEW, NOT the Body facing. This is the assertion that distinguishes
#     view-relative from the old facing-relative behaviour.
# Exits 0 on success, 1 on any failed assertion.

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


# The look direction (forward/right on the XZ plane) for a view yaw. Godot forward
# is local -Z, right is local +X; both rotated by the yaw about UP.
func _view_forward(yaw: float) -> Vector3:
	return Vector3(0.0, 0.0, -1.0).rotated(Vector3.UP, yaw)


func _view_right(yaw: float) -> Vector3:
	return Vector3(1.0, 0.0, 0.0).rotated(Vector3.UP, yaw)


func _assert_wasd_follows(view_yaw: float, tag: String) -> void:
	var fwd := _view_forward(view_yaw)
	var right := _view_right(view_yaw)
	var mW := MovementBasis.character_relative_move(1.0, 0.0, view_yaw).normalized()
	var mS := MovementBasis.character_relative_move(-1.0, 0.0, view_yaw).normalized()
	var mD := MovementBasis.character_relative_move(0.0, 1.0, view_yaw).normalized()
	var mA := MovementBasis.character_relative_move(0.0, -1.0, view_yaw).normalized()
	_check("%s: W drives ALONG the view (dot=%.4f)" % [tag, mW.dot(fwd)], _approx(mW.dot(fwd), 1.0))
	_check("%s: S backpedals opposite the view (dot=%.4f)" % [tag, mS.dot(fwd)], _approx(mS.dot(fwd), -1.0))
	_check("%s: D strafes view-RIGHT (dot=%.4f)" % [tag, mD.dot(right)], _approx(mD.dot(right), 1.0))
	_check("%s: A strafes view-LEFT (dot=%.4f)" % [tag, mA.dot(right)], _approx(mA.dot(right), -1.0))


func _initialize() -> void:
	print("meridian WASD-follows-the-VIEW runtime verify")

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

	# 1) RMB-steer sweep: view and character turn TOGETHER; W follows the view,
	#    which here equals the visible Body facing (no divergence).
	var steers: Array = [60.0, 90.0, -140.0, 120.0]
	for step_dx in steers:
		cam.feed_mouse_motion(step_dx, 0.0, 1)  # 1 = Steer (RMB)
		await _wait(1)
		var st: Dictionary = cam.get_state()
		var cam_yaw: float = st["camera_yaw"]
		var char_yaw: float = st["character_yaw"]
		_check("steer keeps view==facing (cam=%.3f char=%.3f)" % [cam_yaw, char_yaw],
			_approx(cam_yaw, char_yaw))
		_check("Body.rotation.y tracks facing @ %.3f" % char_yaw, _approx(body.rotation.y, char_yaw))
		_assert_wasd_follows(cam_yaw, "steer@%.2f" % cam_yaw)

	# 2) LMB-orbit divergence: orbit turns ONLY the view. W must follow the VIEW,
	#    NOT the (unchanged) Body facing — the view-relative assertion.
	var before: Dictionary = cam.get_state()
	var char_locked: float = before["character_yaw"]
	cam.feed_mouse_motion(110.0, 0.0, 2)  # 2 = Orbit (LMB)
	await _wait(1)
	var after: Dictionary = cam.get_state()
	var view_yaw: float = after["camera_yaw"]
	_check("orbit moved the VIEW (cam %.3f -> %.3f)" % [before["camera_yaw"], view_yaw],
		not _approx(view_yaw, before["camera_yaw"]))
	_check("orbit did NOT turn the character (still %.3f)" % char_locked,
		_approx(after["character_yaw"], char_locked))
	_check("view has DIVERGED from facing (cam=%.3f char=%.3f)" % [view_yaw, char_locked],
		not _approx(view_yaw, char_locked))
	# W follows the orbited view...
	_assert_wasd_follows(view_yaw, "orbit-view")
	# ...and is DISTINCT from the character facing (proves it's the view, not the body).
	var mW_view := MovementBasis.character_relative_move(1.0, 0.0, view_yaw).normalized()
	var body_fwd := _view_forward(char_locked)
	_check("W follows the VIEW, not the body facing (dot=%.4f < 0.99)" % mW_view.dot(body_fwd),
		mW_view.dot(body_fwd) < 0.99)

	# 3) Diagonal speed: the controller core must normalize so W+D is not faster.
	var mover := MeridianMovementController.new()
	mover.reset(Vector3.ZERO, 0.0)
	var t := 0
	var moveW: Vector3 = MovementBasis.character_relative_move(1.0, 0.0, view_yaw)
	mover.predict(moveW, false, false, view_yaw, t)
	t += 50
	mover.predict(moveW, false, false, view_yaw, t)
	var spW := Vector2(mover.get_predicted_velocity().x, mover.get_predicted_velocity().z).length()
	var mover2 := MeridianMovementController.new()
	mover2.reset(Vector3.ZERO, 0.0)
	var moveWD: Vector3 = MovementBasis.character_relative_move(1.0, 1.0, view_yaw)
	t = 0
	mover2.predict(moveWD, false, false, view_yaw, t)
	t += 50
	mover2.predict(moveWD, false, false, view_yaw, t)
	var spWD := Vector2(mover2.get_predicted_velocity().x, mover2.get_predicted_velocity().z).length()
	print("  W speed=%.4f  W+D speed=%.4f" % [spW, spWD])
	_check("diagonal (W+D) is NOT faster than W (core normalizes)",
		_approx(spWD, spW, 1e-2) and spW > 0.0)

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)
