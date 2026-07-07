# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the third-person WoW
# camera (issue #105). NOT a shipped scene: this is a SceneTree script run under
# `godot --headless --script res://scenes/world/camera_verify.gd` so CI / a dev
# box can prove the C++ MeridianTpsCamera class loads, the SpringArm3D + Camera3D
# rig instantiates, and the full runtime path behaves — steer vs orbit, wheel
# zoom clamps, and the collision boom pulling in on an obstruction and springing
# back out when it clears — all with REAL physics frames (get_hit_length), no
# render, no mouse. Exits 0 on success, 1 on any failed assertion.
#
# The interactive playtest lives in camera_demo.tscn (see the PR); this script is
# the automatable evidence.

extends SceneTree

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _approx(a: float, b: float, eps := 0.01) -> bool:
	return abs(a - b) <= eps


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("meridian tps camera RUNTIME verify (#105)")

	# --- Class loads? ---
	var loaded := ClassDB.class_exists("MeridianTpsCamera")
	_check("MeridianTpsCamera class registered", loaded)
	if not loaded:
		quit(1)
		return

	# --- Build a minimal world: ground + player + camera ---
	var world := Node3D.new()
	world.name = "World"
	root.add_child(world)

	# Ground plane (StaticBody3D, physics layer 1).
	var ground := StaticBody3D.new()
	ground.name = "Ground"
	var gcol := CollisionShape3D.new()
	var gbox := BoxShape3D.new()
	gbox.size = Vector3(40, 1, 40)
	gcol.shape = gbox
	ground.add_child(gcol)
	ground.position = Vector3(0, -0.5, 0)
	world.add_child(ground)

	# Player anchor (NOT yaw-rotated) with a visible body + the camera.
	var player := Node3D.new()
	player.name = "Player"
	world.add_child(player)

	var body := MeshInstance3D.new()
	body.name = "Body"
	body.mesh = CapsuleMesh.new()
	player.add_child(body)

	var cam := ClassDB.instantiate("MeridianTpsCamera") as Node3D
	cam.name = "TpsCamera"
	cam.set("capture_mouse", false)          # headless: no cursor grab
	cam.set("yaw_target_path", NodePath("../Body"))
	player.add_child(cam)

	# --- Rig instantiated without errors? ---
	await _wait(2)
	var pivot: Node = cam.get_node_or_null("PitchPivot")
	_check("PitchPivot child created", pivot != null)
	_check("SpringArm3D probe created", cam.get_node_or_null("PitchPivot/BoomProbe") != null)
	var cam3d: Camera3D = cam.get_node_or_null("PitchPivot/Camera3D") as Camera3D
	_check("Camera3D created", cam3d != null)
	_check("Camera3D is current", cam3d != null and cam3d.current)

	# --- Boom settles unobstructed at the default zoom ---
	await _wait(30)
	var st: Dictionary = cam.get_state()
	print("  unobstructed state: ", st)
	_check("boom settled at zoom_default (~6)", _approx(st["boom_length"], 6.0, 0.05))
	var behind_z: float = (cam3d.global_position - player.global_position).z
	_check("camera sits BEHIND the character (+Z)", behind_z > 0.5)

	# --- Collision boom: wall directly behind the character pulls the camera IN ---
	var wall := StaticBody3D.new()
	wall.name = "Wall"
	var wcol := CollisionShape3D.new()
	var wbox := BoxShape3D.new()
	wbox.size = Vector3(6, 6, 0.5)
	wcol.shape = wbox
	wall.add_child(wcol)
	# Behind = +Z of the (unrotated, yaw=0) player frame, where the camera sits.
	wall.position = Vector3(0, 1.6, 3.5)
	world.add_child(wall)

	await _wait(15)
	st = cam.get_state()
	print("  obstructed state:   ", st)
	_check("boom pulled IN by the wall (< 4 m)", st["boom_length"] < 4.0)
	_check("boom pulled to roughly the hit length",
		_approx(st["boom_length"], st["hit_length"], 0.2))

	# --- Spring-out: remove the wall, boom eases back toward the zoom length ---
	wall.queue_free()
	await _wait(3)
	var short_boom: float = cam.get_state()["boom_length"]
	await _wait(90)
	st = cam.get_state()
	print("  recovered state:    ", st)
	_check("boom springs back OUT after the wall clears", st["boom_length"] > short_boom + 1.0)
	_check("boom recovers to the zoom length (~6)", _approx(st["boom_length"], 6.0, 0.1))

	# --- Steer (RMB): character + camera turn together ---
	cam.feed_mouse_motion(100.0, 0.0, 1)      # mode 1 = steer
	await _wait(1)
	st = cam.get_state()
	_check("steer turned the character", abs(st["character_yaw"]) > 0.1)
	_check("steer keeps camera locked behind (camera_yaw == character_yaw)",
		_approx(st["camera_yaw"], st["character_yaw"], 1e-4))
	_check("Body node actually rotated to face character_yaw",
		_approx(body.rotation.y, st["character_yaw"], 1e-3))

	# --- Orbit (LMB): camera orbits, character facing frozen ---
	var char_before: float = st["character_yaw"]
	cam.feed_mouse_motion(80.0, 0.0, 2)       # mode 2 = orbit
	await _wait(1)
	st = cam.get_state()
	_check("orbit left the character facing unchanged", _approx(st["character_yaw"], char_before, 1e-4))
	_check("orbit moved the camera yaw", not _approx(st["camera_yaw"], char_before, 1e-3))

	# --- Zoom clamp ---
	cam.feed_zoom(100)                        # way out
	_check("zoom clamps at zoom_max (20)", _approx(cam.get_zoom(), 20.0, 1e-3))
	cam.feed_zoom(-1000)                      # way in
	_check("zoom clamps at zoom_min (1.5)", _approx(cam.get_zoom(), 1.5, 1e-3))

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)
