# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the NETWORKED world scene
# (issue #301). NOT a shipped scene: run under
#   godot --headless --script res://scenes/world/world_verify.gd
# so CI / a dev box can prove, with REAL physics frames and NO display and NO server:
#   * the net/sim GDExtension classes load (MeridianNetThread, MeridianRemoteInterpolator,
#     MeridianMovementController, MeridianTpsCamera);
#   * the #301 GUI-net-path helpers exist + behave: build_movement_intent_frame()
#     encodes a ready-to-send IF-2 frame, and decode_entity_frame() safely rejects a
#     non-entity / garbage payload;
#   * world.tscn WARM-LOADS in the OFFLINE (no-session) path — it instantiates, builds
#     the local player + camera + HUD, and survives physics frames without touching a
#     socket (the seed-then-warm scene-load check, #283).
# The LIVE see-a-bot-move proof of the same net path is
# client/test/run_client_sees_bot_it.sh; the on-screen watch is
# scripts/dev/demo-networked.sh. Exits 0 on success, 1 on any failed assertion.

extends SceneTree

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("meridian networked world RUNTIME verify (#301)")

	# --- 1. The net/sim classes load. ---
	for cls in ["MeridianNetThread", "MeridianRemoteInterpolator",
			"MeridianMovementController", "MeridianTpsCamera"]:
		_check("%s class registered" % cls, ClassDB.class_exists(cls))
	if _fails > 0:
		quit(1)
		return

	# --- 2. The #301 GUI-net-path helpers on MeridianNetThread. ---
	var net := MeridianNetThread.new()
	_check("has decode_entity_frame", net.has_method("decode_entity_frame"))
	_check("has build_movement_intent_frame", net.has_method("build_movement_intent_frame"))

	# build_movement_intent_frame encodes a real IF-2 frame (u16 opcode + u64 seq +
	# FlatBuffer body → well over the 10-byte header).
	var intent := {
		"seq": 7, "state_flags": 0x9, "x": 64.0, "y": 0.0, "z": 66.0,
		"orientation": 1.5, "client_time_ms": 123456,
	}
	var frame: PackedByteArray = net.build_movement_intent_frame(intent)
	_check("build_movement_intent_frame produces a framed intent", frame.size() > 10)
	# opcode 0x1001 (MovementIntent) little-endian in the first two bytes.
	_check("movement-intent opcode 0x1001 encoded", frame.size() >= 2
		and frame[0] == 0x01 and frame[1] == 0x10)
	# A malformed intent Dictionary yields an empty frame (no crash).
	_check("empty intent → empty frame", net.build_movement_intent_frame({}).size() == 0)

	# decode_entity_frame safely rejects a non-entity opcode + garbage payload.
	var d_clock: Dictionary = net.decode_entity_frame(0x0004, PackedByteArray())  # ClockSync
	_check("non-entity opcode → kind ''", String(d_clock.get("kind", "x")) == "")
	var d_bad: Dictionary = net.decode_entity_frame(0x2001,
		PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))  # EntityEnter opcode, garbage body
	_check("garbage entity payload → kind ''", String(d_bad.get("kind", "x")) == "")

	# --- 3. WARM-LOAD world.tscn in the OFFLINE path (no session → local sandbox). ---
	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		quit(1)
		return
	var world := packed.instantiate()
	# Configure with NO session (offline) so _ready() never opens a socket.
	world.configure({}, {"name": "Verifier"})
	root.add_child(world)
	await _wait(5)  # real physics frames — _physics_process runs, no server, no crash

	_check("world instantiated as Node3D", world is Node3D)
	_check("local Player built", world.get_node_or_null("Player") != null)
	_check("TpsCamera built on player",
		world.get_node_or_null("Player/TpsCamera") != null)
	_check("Remotes container built", world.get_node_or_null("Remotes") != null)
	_check("HUD built", world.get_node_or_null("HUD") != null)
	_check("no remote entities offline", world.get_node("Remotes").get_child_count() == 0)

	world.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("world verify: ALL PASS")
		quit(0)
	else:
		print("world verify: %d FAILURE(S)" % _fails)
		quit(1)
