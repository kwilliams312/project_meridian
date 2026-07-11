# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for NAMEPLATES (CMB-03, #535;
# combat-presentation epic #23). NOT a shipped scene: run under
#   godot --headless --script res://scenes/world/nameplate_verify.gd
# so CI / a dev box can prove, with REAL frames and NO display and NO server, that:
#   * the world scene builds the POOLED nameplate manager and preallocates its plates
#     (no per-spawn allocation — the free pool is full at rest, nothing active);
#   * an entering remote entity gets a nameplate ATTACHED over its Node3D (a child of the
#     entity node, so it tracks the entity), seeded with the ENTITY_ENTER name + health;
#   * the plate is billboarded (faces the camera) — the name label + both bar quads;
#   * the health bar TRACKS a VITALS_UPDATE published on the SAME event-bus seam the unit
#     frames read (entity_vitals_changed) — the fill shrinks to the new current/max ratio;
#   * an ENTITY_LEAVE RECYCLES the plate back into the pool (reuse, not a leak) and the
#     entity node is gone;
#   * the LOCAL player's own nameplate is SUPPRESSED — self-vitals never spawn a plate;
#   * the pool never grows past POOL_SIZE under a churn of unique entities;
#   * the distance-fade alpha plumbing drives the plate transparency.
# It drives world.tscn in its OFFLINE (no-session) path — no socket is ever opened.
# Exits 0 on success, 1 on any failed assertion — same shape as combat_text_verify.gd.

extends SceneTree

const Nameplate := preload("res://scenes/world/nameplate.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("meridian NAMEPLATES runtime verify (#535)")

	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		quit(1)
		return
	var world = packed.instantiate()
	# OFFLINE (no session) — _ready() builds the player/camera/HUD/event bus + the pooled
	# nameplate manager, never a socket.
	world.configure({}, {"name": "Verifier", "level": 1, "class": 1})
	root.add_child(world)
	await _wait(5)  # real frames — the scene builds and the pool preallocates

	var bus = world._bus
	var plates = world._nameplates
	_check("event bus built", bus != null)
	_check("nameplate manager built", plates != null)
	if plates == null:
		world.queue_free()
		quit(1)
		return

	# --- Pool is preallocated and idle at rest (NO per-spawn allocation). ---------------
	_check("pool preallocated to POOL_SIZE plates", plates.free_count() == plates.pool_size())
	_check("nothing active at rest", plates.active_count() == 0)
	var plate_children := 0
	for child in plates.get_children():
		if child.get_script() == Nameplate:
			plate_children += 1
	_check("all pool plates exist as child nodes", plate_children == plates.pool_size())

	# --- An entering remote gets a nameplate attached over its node, seeded from ENTER. --
	var mob_guid := 0x64
	var mob_pos := Vector3(64.0, 0.0, 72.0)
	var enter := {
		"guid": mob_guid, "position": mob_pos, "char_class": 2,
		"name": "Gnasher", "level": 3,
		"health": 80, "max_health": 100, "power": 0, "max_power": 0, "power_type": 0,
	}
	# Match the real _on_entity_frame ordering: publish to the bus, then spawn the node.
	bus.publish_entity_enter(mob_guid, enter)
	world._spawn_remote(mob_guid, enter)
	await _wait(2)
	_check("remote node spawned", world._remote_nodes.has(mob_guid))
	_check("nameplate active for the entering entity", plates.has(mob_guid))
	_check("one plate active, one fewer free",
		plates.active_count() == 1 and plates.free_count() == plates.pool_size() - 1)

	var mob_node = world._remote_nodes[mob_guid]
	var plate = plates.plate_for(mob_guid)
	_check("plate is a child of the entity node (tracks it)",
		plate != null and plate.get_parent() == mob_node)
	_check("plate shows the ENTITY_ENTER name", plate != null and plate.name_text() == "Gnasher")
	_check("plate is visible", plate != null and plate.visible)
	_check("health bar tracks the ENTER baseline (80/100 = 0.8)",
		plate != null and absf(plate.health_ratio() - 0.8) < 0.001)
	# The fill quad shrinks left-anchored to the ratio (a hard-empty bar is width 0).
	_check("health fill width matches the ratio",
		plate != null and absf(plate.fill_width() - Nameplate.BAR_WIDTH * 0.8) < 0.001)

	# --- Billboarding: the name label + both bar quads face the camera. -----------------
	var name_label = plate.get_node_or_null("Name")
	var bar_bg = plate.get_node_or_null("HealthBarBg")
	var bar_fill = plate.get_node_or_null("HealthBarFill")
	_check("name label billboards", name_label != null
		and name_label.billboard == BaseMaterial3D.BILLBOARD_ENABLED)
	_check("health bar bg billboards", bar_bg != null
		and bar_bg.material_override.billboard_mode == BaseMaterial3D.BILLBOARD_ENABLED)
	_check("health bar fill billboards", bar_fill != null
		and bar_fill.material_override.billboard_mode == BaseMaterial3D.BILLBOARD_ENABLED)

	# --- The health bar TRACKS a VITALS_UPDATE via the event-bus seam. ------------------
	bus.publish_vitals_update(mob_guid, {"health": 30, "max_health": 100})
	await _wait(1)
	_check("health bar tracked the VITALS_UPDATE (30/100 = 0.3)",
		absf(plate.health_ratio() - 0.3) < 0.001)
	_check("health fill shrank to the new ratio",
		absf(plate.fill_width() - Nameplate.BAR_WIDTH * 0.3) < 0.001)
	# A raised max (e.g. a level-up) is honoured too — ratio recomputes from the new cap.
	bus.publish_vitals_update(mob_guid, {"health": 60, "max_health": 120})
	await _wait(1)
	_check("health bar tracks a changed max (60/120 = 0.5)",
		absf(plate.health_ratio() - 0.5) < 0.001)

	# --- Distance-fade alpha plumbing drives the plate transparency. --------------------
	plate.set_alpha(0.4)
	_check("plate alpha is driven by set_alpha", absf(plate.alpha() - 0.4) < 0.001)
	plate.set_alpha(1.0)

	# --- The LOCAL player's own nameplate is SUPPRESSED (self-vitals spawn no plate). ---
	world._my_guid = 0x9001
	bus.publish_vitals_update(0x9001, {"health": 50, "max_health": 50})
	await _wait(1)
	_check("no nameplate for the local player", not plates.has(0x9001))
	_check("local self-vitals did not spawn a plate", plates.active_count() == 1)

	# --- ENTITY_LEAVE recycles the plate back into the pool (reuse, not a leak). ---------
	world._despawn_remote(mob_guid)
	await _wait(2)
	_check("entity node removed on leave", not world._remote_nodes.has(mob_guid))
	_check("nameplate recycled on leave", not plates.has(mob_guid))
	_check("plate returned to the pool", plates.active_count() == 0
		and plates.free_count() == plates.pool_size())
	_check("recycled plate reparented back under the manager", plate.get_parent() == plates)
	_check("recycled plate hidden", not plate.visible)

	# --- Under a churn of unique entities the pool never grows past POOL_SIZE. -----------
	var churn: int = plates.pool_size() + 6
	for n in range(churn):
		var g := 0x1000 + n
		var d := {"position": Vector3(64.0, 0.0, 70.0 + n), "char_class": 1,
			"name": "Mob%d" % n, "health": 100, "max_health": 100}
		bus.publish_entity_enter(g, d)
		world._spawn_remote(g, d)
	await _wait(2)
	_check("churn never exceeds the pool size", plates.active_count() == plates.pool_size())
	# Invariant proving no allocation happened: every plate is either attached (active) or in
	# the free pool — active + free is ALWAYS exactly POOL_SIZE, never more (the oldest is
	# recycled early rather than a new plate being allocated).
	_check("churn allocated no extra plates (active + free == pool)",
		plates.active_count() + plates.free_count() == plates.pool_size())

	world.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("nameplate verify: ALL PASS")
		quit(0)
	else:
		print("nameplate verify: %d FAILURE(S)" % _fails)
		quit(1)
