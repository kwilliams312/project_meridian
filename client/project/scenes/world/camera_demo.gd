# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — interactive playtest scene for the third-person WoW camera
# (issue #105). Attach to the root of camera_demo.tscn. Builds a tiny sandbox in
# code (ground, a few collision pillars, a lit capsule "character") and drops a
# C++ MeridianTpsCamera on the player so a human can verify the feel:
#
#   * Hold RIGHT-mouse + move  -> STEER: the capsule + camera turn together.
#   * Hold LEFT-mouse  + move  -> ORBIT: the camera circles; the capsule holds.
#   * Mouse WHEEL              -> zoom the boom in / out (clamped).
#   * Walk the camera into a pillar (orbit around behind one) -> the boom pulls
#     in to avoid clipping and springs back out when the view clears.
#   * ESC releases the captured mouse cursor.
#
# Run:  godot --path project --scene res://scenes/world/camera_demo.tscn
# (or set it as the main scene and press Play). The automatable, no-render
# counterpart is camera_verify.gd.

extends Node3D


func _ready() -> void:
	_build_environment()
	_build_ground()
	_build_pillars()
	_build_player_and_camera()
	print("[camera_demo] RMB = steer, LMB = orbit, wheel = zoom, ESC = free cursor")


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func _build_environment() -> void:
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-55.0), deg_to_rad(35.0), 0.0)
	add_child(light)

	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.36, 0.45, 0.55)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.4, 0.4, 0.45)
	env.ambient_light_energy = 0.6
	we.environment = env
	add_child(we)


func _build_ground() -> void:
	var ground := StaticBody3D.new()
	ground.name = "Ground"
	var col := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(60, 1, 60)
	col.shape = box
	ground.add_child(col)
	var mesh := MeshInstance3D.new()
	var pm := BoxMesh.new()
	pm.size = Vector3(60, 1, 60)
	mesh.mesh = pm
	ground.add_child(mesh)
	ground.position = Vector3(0, -0.5, 0)
	add_child(ground)


func _build_pillars() -> void:
	# A ring of pillars so orbiting/steering behind one exercises the collision boom.
	for i in range(6):
		var a := float(i) / 6.0 * TAU
		var pillar := StaticBody3D.new()
		pillar.name = "Pillar%d" % i
		var col := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = Vector3(1.2, 5, 1.2)
		col.shape = box
		pillar.add_child(col)
		var mesh := MeshInstance3D.new()
		var pm := BoxMesh.new()
		pm.size = Vector3(1.2, 5, 1.2)
		mesh.mesh = pm
		pillar.add_child(mesh)
		pillar.position = Vector3(cos(a) * 5.0, 2.5, sin(a) * 5.0)
		add_child(pillar)


func _build_player_and_camera() -> void:
	# Position anchor (NOT yaw-rotated) — see MeridianTpsCamera header.
	var player := Node3D.new()
	player.name = "Player"
	add_child(player)

	# Visible "character" the camera turns during steer (yaw_target).
	var body := MeshInstance3D.new()
	body.name = "Body"
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	body.mesh = capsule
	body.position = Vector3(0, 0.9, 0)
	# A "nose" so facing is obvious while steering.
	var nose := MeshInstance3D.new()
	var nm := BoxMesh.new()
	nm.size = Vector3(0.2, 0.2, 0.5)
	nose.mesh = nm
	nose.position = Vector3(0, 0.2, -0.5)  # -Z is forward in Godot
	body.add_child(nose)
	player.add_child(body)

	var cam := ClassDB.instantiate("MeridianTpsCamera") as Node3D
	cam.name = "TpsCamera"
	cam.set("yaw_target_path", NodePath("../Body"))
	player.add_child(cam)
