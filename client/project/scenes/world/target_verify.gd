# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for TARGET SELECTION (CMB-01, #496,
# combat-presentation epic #23). NOT a shipped scene: run under
#   godot --headless --script res://scenes/world/target_verify.gd
# so CI / a dev box can prove, with REAL physics frames and NO display and NO server:
#   * a spawned remote entity carries a PICKABLE collider on the dedicated target layer,
#     and a physics ray resolves that entity's guid (the core of click-to-target);
#   * TAB cycles the in-AoI entities NEAREST-FIRST through the event bus (set_target);
#   * on set_target the TARGET UNIT FRAME shows the target's name + health/power vitals
#     (from bus.target_vitals()) and updates LIVE from a VITALS_UPDATE delta;
#   * clearing the target (Escape / empty click) hides the frame AND the selection ring;
#   * the selection ring shows for an in-AoI remote target and hides when cleared;
#   * CAST_REQUEST carries the current target_guid() so a targeted attack hits the mob.
# It drives world.tscn in its OFFLINE (no-session) path — no socket is ever opened.
# Exits 0 on success, 1 on any failed assertion — same shape as world_verify.gd.

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
	print("meridian target-selection RUNTIME verify (#496)")

	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		quit(1)
		return
	var world = packed.instantiate()
	# OFFLINE (no session) — _ready() builds the player/camera/HUD/event bus, never a socket.
	world.configure({}, {"name": "Verifier", "level": 1, "class": 1})
	root.add_child(world)
	await _wait(5)  # real physics frames — the scene builds + the picker colliders register

	var bus = world._bus
	var hud = world._hud
	_check("event bus built", bus != null)
	_check("HUD built", hud != null)
	_check("active Camera3D present (click-pick projection source)",
		world.get_viewport().get_camera_3d() != null)

	# --- Spawn two remote entities (a near mob and a far mob), with #430 vitals. --------
	# Godot-space world positions (wire (64,64) player → Godot (64,0,64)); the near mob is
	# 6 m away, the far mob 20 m away, so nearest-first is unambiguous.
	var near_guid := 0x51
	var far_guid := 0x77
	world._spawn_remote(near_guid, {"position": Vector3(64.0, 0.0, 70.0), "char_class": 2})
	world._spawn_remote(far_guid, {"position": Vector3(64.0, 0.0, 84.0), "char_class": 3})
	bus.publish_entity_enter(near_guid, {
		"name": "Kobold Miner", "level": 3, "char_class": 0,
		"health": 90, "max_health": 120,
		"power": 0, "max_power": 0, "power_type": 0,
	})
	bus.publish_entity_enter(far_guid, {
		"name": "Ember Wisp", "level": 4, "char_class": 0,
		"health": 60, "max_health": 60,
		"power": 50, "max_power": 100, "power_type": 1,  # MANA
	})
	await _wait(3)  # let the StaticBody3D pickers register in the physics space
	_check("two remote nodes spawned", world._remote_nodes.size() == 2)

	# --- Click-to-target: a physics ray resolves the entity guid from its collider. -----
	# Cast a vertical ray straight through the NEAR mob's capsule on the TARGET layer; it
	# must resolve near_guid (the ground body is on layer 1 and is correctly ignored).
	var hit_guid: int = world._raycast_target_guid(Vector3(64.0, 5.0, 70.0), Vector3(64.0, -5.0, 70.0))
	_check("ray through the near mob resolves its guid", hit_guid == near_guid)
	# A ray through empty space resolves nothing (→ clears on a real click).
	var miss_guid: int = world._raycast_target_guid(Vector3(0.0, 5.0, 0.0), Vector3(0.0, -5.0, 0.0))
	_check("ray through empty space resolves 0", miss_guid == 0)
	# The far mob resolves too (proves per-entity metadata, not a fluke).
	var far_hit: int = world._raycast_target_guid(Vector3(64.0, 5.0, 84.0), Vector3(64.0, -5.0, 84.0))
	_check("ray through the far mob resolves its guid", far_hit == far_guid)

	# Selecting from a pick flows through the bus like a real click.
	bus.set_target(hit_guid)
	await _wait(1)
	_check("click selected the near mob", bus.target_guid() == near_guid)

	# --- Target unit frame shows the target's vitals from the bus. ----------------------
	var tf = hud._target_frame
	_check("target frame shown on target", tf.visible)
	_check("target frame reflects target health (~90/120)",
		absf(tf._health_fill.size.x - tf.BAR_W * (90.0 / 120.0)) < 1.0)

	# --- Live VITALS_UPDATE on the target updates the bar (takes damage). ----------------
	bus.publish_vitals_update(near_guid, {
		"health": 30, "max_health": 120,
		"power": 0, "max_power": 0, "power_type": 0,
	})
	await _wait(1)
	_check("target frame updates live on VITALS_UPDATE (~30/120)",
		absf(tf._health_fill.size.x - tf.BAR_W * (30.0 / 120.0)) < 1.0)

	# --- Selection ring shows for the remote target, hides on clear. --------------------
	var ring = world._target_ring
	_check("selection ring visible for the remote target", ring != null and ring.visible)
	_check("selection ring sits at the target's feet",
		ring != null and ring.position.distance_to(Vector3(64.0, 0.06, 70.0)) < 0.2)

	# --- CAST_REQUEST carries the current target_guid() (pickaxe_slam hits the mob). ----
	bus.seed_abilities([{
		"ability_id": 2, "name": "Pickaxe Slam", "icon_id": 2, "hotkey": 1,
		"cast_ms": 0, "triggers_gcd": true, "resource_type": 0, "resource_cost": 0, "range_m": 5.0,
	}])
	var cast_sink: Dictionary = {"target": -1, "ability": -1}
	bus.cast_requested.connect(func(aid, tguid, _t): cast_sink["ability"] = aid; cast_sink["target"] = tguid)
	var issued: bool = bus.request_cast(2, 1000)
	_check("cast issued", issued)
	_check("CAST_REQUEST carries the selected target guid",
		int(cast_sink["target"]) == near_guid and int(cast_sink["ability"]) == 2)

	# --- TAB cycles nearest-first; then wraps. ------------------------------------------
	# Clear, then TAB from nothing → the NEAREST (near mob); TAB again → the far mob; TAB
	# again → wraps back to the near mob.
	bus.set_target(0)
	await _wait(1)
	_check("clear hid the target frame", not tf.visible)
	_check("clear hid the selection ring", not ring.visible)

	world._cycle_target()
	_check("TAB from nothing selects the nearest mob", bus.target_guid() == near_guid)
	world._cycle_target()
	_check("TAB again advances to the far mob", bus.target_guid() == far_guid)
	world._cycle_target()
	_check("TAB wraps back to the nearest mob", bus.target_guid() == near_guid)

	# --- EntityLeave on the target clears it (frame + ring vanish). ----------------------
	bus.publish_entity_leave(near_guid)
	await _wait(1)
	_check("leave cleared the target", bus.target_guid() == 0)
	_check("leave hid the target frame", not tf.visible)
	_check("leave hid the selection ring", not ring.visible)

	world.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("target verify: ALL PASS")
		quit(0)
	else:
		print("target verify: %d FAILURE(S)" % _fails)
		quit(1)
