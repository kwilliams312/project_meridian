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

# MeridianContentDB (#477) by PATH — standalone --script mode has no autoloads; the
# staged core pack must be loaded so _spawn_remote's assembled branch has real content
# (②/T4, #541). preload is immune to a stale global class cache.
const ContentDbScript := preload("res://content/content_db.gd")

# The 8 blockout geoset regions (tools/blender/meridian_rig generate_blockout.py) — the
# assembled body must show all 8 (②/T4 scene-tree proof).
const REGIONS: Array = [
	"feet", "forearms", "hands", "head", "hips_legs", "lower_legs", "torso", "waist",
]

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

	# --- 4. ②/T4 (#541): _spawn_remote renders an AssembledCharacter when the frame
	# carries appearance, and the class-colored capsule otherwise (the fallback seam). ---
	var db = ContentDbScript.instance()
	db.load_from("res://meridian/core")  # the staged pack the assembler builds from
	_check("staged core pack loaded for the assembler", db.is_loaded())
	var pickaxe: int = db.numeric_id_for("core:item.rusty_pickaxe")

	# Assembled branch: a player EntityEnter with appearance + a socketed pickaxe.
	var d_assembled := {
		"kind": "enter", "guid": 7001, "position": Vector3(2, 0, 2), "orientation": 0.0,
		"char_class": 1, "name": "Ardent", "race": 1, "sex": 0,
		"appearance": {"v": 1, "hair": 1, "face": 1, "skin": 1},
		"equipment": [{"slot": 1, "item_template": pickaxe, "dyes": []}],
	}
	world._spawn_remote(7001, d_assembled)
	var rnode: Node3D = world.get_node_or_null("Remotes/Remote_7001")
	_check("assembled remote node spawned", rnode != null)
	var abody: Node = rnode.get_node_or_null("Body") if rnode != null else null
	_check("remote body is an AssembledCharacter (not a capsule)",
		abody != null and abody.has_method("body_skeleton") and abody.has_method("geoset_node"))
	if abody != null and abody.has_method("body_skeleton"):
		var skel: Skeleton3D = abody.body_skeleton()
		_check("assembled remote has the 63-bone canonical skeleton",
			skel != null and skel.get_bone_count() == 63)
		var geosets := 0
		for region in REGIONS:
			var g: MeshInstance3D = abody.geoset_node(region)
			if g != null and g.visible:
				geosets += 1
		_check("assembled remote shows all 8 geoset meshes", geosets == 8)
		var eq: Array = abody.equipped_nodes(1)
		_check("pickaxe socketed on socket_main_hand of the assembled remote",
			eq.size() == 1 and eq[0] is BoneAttachment3D
			and String(eq[0].bone_name) == "socket_main_hand")

	# Fallback branch: an EntityEnter WITHOUT appearance (NPC / old server) → capsule.
	var d_capsule := {
		"kind": "enter", "guid": 7002, "position": Vector3(3, 0, 3), "orientation": 0.0,
		"char_class": 2, "name": "Capsule",
	}
	world._spawn_remote(7002, d_capsule)
	var cnode: Node3D = world.get_node_or_null("Remotes/Remote_7002")
	var cbody: Node = cnode.get_node_or_null("Body") if cnode != null else null
	_check("no-appearance remote falls back to a class-colored capsule",
		cbody is MeshInstance3D and (cbody as MeshInstance3D).mesh is CapsuleMesh)

	world.queue_free()
	await _wait(1)

	# --- 5. ②/T4: the LOCAL player body assembles from char-select data at ENTER_WORLD
	# when the character carries appearance (seeded here as the login handoff would). ---
	var packed2 := load("res://scenes/world/world.tscn") as PackedScene
	var world2 := packed2.instantiate()
	world2.configure({}, {
		"name": "Ardent", "class": 1, "race": 1,
		"appearance": {"v": 1, "hair": 1, "face": 1, "skin": 1},
	})
	root.add_child(world2)
	await _wait(3)
	var lbody: Node = world2.get_node_or_null("Player/Body")
	_check("local player body is an AssembledCharacter when the char carries appearance",
		lbody != null and lbody.has_method("body_skeleton"))
	world2.queue_free()
	await _wait(1)

	# The offline warm-load character (no appearance) still gets the capsule fallback.
	var packed3 := load("res://scenes/world/world.tscn") as PackedScene
	var world3 := packed3.instantiate()
	world3.configure({}, {"name": "Verifier"})
	root.add_child(world3)
	await _wait(3)
	var lbody3: Node = world3.get_node_or_null("Player/Body")
	_check("local player body is a capsule when the char carries no appearance",
		lbody3 is MeshInstance3D and (lbody3 as MeshInstance3D).mesh is CapsuleMesh)
	world3.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("world verify: ALL PASS")
		quit(0)
	else:
		print("world verify: %d FAILURE(S)" % _fails)
		quit(1)
