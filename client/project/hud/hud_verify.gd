# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the HUD foundation (UI-01,
# #431): the MVVM event bus, the unit-frame view, the power-color table, and the HUD
# binding. NOT a shipped scene: run it as
#   godot --headless --script res://hud/hud_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the event bus stores + re-emits server unit state (EntityEnter snapshot +
#     VITALS_UPDATE delta merge, name preservation, local/target binding, leave);
#   * a unit frame renders vitals into its bars/labels (health ratio, power bar shown
#     for a real pool and hidden for PowerType NONE);
#   * the HUD binds the player + target frames to the bus and updates on its signals;
#   * MeridianNetThread.decode_entity_frame() safely handles the new VITALS_UPDATE
#     opcode (0x2004). The full wire round-trip is proven by the C++ ctest
#     (client/net/test — clientnet's VitalsUpdate + EntityEnter-vitals cases).
# Exits 0 on success, 1 on any failed assertion — same shape as world_verify.gd.
extends SceneTree

const EventBus := preload("res://hud/event_bus.gd")
const UnitFrame := preload("res://hud/unit_frame.gd")
const Hud := preload("res://hud/hud.gd")
const PowerColors := preload("res://hud/power_colors.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian HUD foundation RUNTIME verify (#431)")

	_verify_power_colors()
	_verify_event_bus()
	await _verify_unit_frame()
	await _verify_hud_binding()
	_verify_decode_safety()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


func _verify_power_colors() -> void:
	print("[power_colors]")
	_check("MANA has a distinct color", PowerColors.color_for(PowerColors.MANA) != PowerColors.color_for(PowerColors.RAGE))
	_check("has_power true for MANA/100", PowerColors.has_power(PowerColors.MANA, 100))
	_check("has_power false for NONE", not PowerColors.has_power(PowerColors.NONE, 0))
	_check("has_power false for zero cap", not PowerColors.has_power(PowerColors.MANA, 0))


func _verify_event_bus() -> void:
	print("[event_bus]")
	var bus = EventBus.new()

	# EntityEnter snapshot lands + is readable by guid. (Capture into a container the
	# lambda MUTATES — GDScript closures capture locals by value, so a bare `last = v`
	# reassignment would not propagate out.)
	var sink: Dictionary = {"last": {}}
	bus.entity_vitals_changed.connect(func(_g, v): sink["last"] = v)
	bus.publish_entity_enter(0x2A, {
		"name": "Bot-7", "level": 5, "char_class": 3,
		"health": 800, "max_health": 1000,
		"power": 40, "max_power": 100, "power_type": PowerColors.ENERGY,
	})
	_check("enter stored", bus.has_entity(0x2A))
	var v: Dictionary = bus.get_vitals(0x2A)
	_check("enter name", String(v["name"]) == "Bot-7")
	_check("enter health", int(v["health"]) == 800 and int(v["max_health"]) == 1000)
	_check("enter power_type", int(v["power_type"]) == PowerColors.ENERGY)
	_check("signal carried the record", int((sink["last"] as Dictionary).get("health", 0)) == 800)

	# VITALS_UPDATE delta merges onto the snapshot WITHOUT dropping the name (the
	# delta has no name field). Health drops, level bumps.
	bus.publish_vitals_update(0x2A, {
		"health": 250, "max_health": 1000,
		"power": 10, "max_power": 100, "power_type": PowerColors.ENERGY, "level": 6,
	})
	v = bus.get_vitals(0x2A)
	_check("delta merged health", int(v["health"]) == 250)
	_check("delta preserved name", String(v["name"]) == "Bot-7")
	_check("delta bumped level", int(v["level"]) == 6)

	# get_vitals for an unknown guid returns a safe all-zero record (no has()-guards).
	var empty: Dictionary = bus.get_vitals(0x999)
	_check("unknown guid → zero record", int(empty["health"]) == 0 and String(empty["name"]) == "")

	# Local-player identity: seed (name/level from char-select), then bind.
	bus.seed_identity(0x2A, "", 0, 0)  # empty seed must NOT clobber the wire name
	_check("empty seed keeps wire name", String(bus.get_vitals(0x2A)["name"]) == "Bot-7")
	bus.seed_identity(0x100, "Aelric", 3, 1)
	bus.set_local_player(0x100)
	_check("local guid set", bus.local_guid() == 0x100)
	_check("local vitals seeded", String(bus.local_vitals()["name"]) == "Aelric")

	# Target binding + clear-on-leave.
	var target_events: Array = []
	bus.target_changed.connect(func(g): target_events.append(g))
	bus.set_target(0x2A)
	_check("target guid set", bus.target_guid() == 0x2A)
	_check("target vitals resolve", String(bus.target_vitals()["name"]) == "Bot-7")
	bus.publish_entity_leave(0x2A)
	_check("leave dropped entity", not bus.has_entity(0x2A))
	_check("leave cleared target", bus.target_guid() == 0)
	_check("target_changed fired to 0", target_events.size() >= 1 and int(target_events[-1]) == 0)


func _verify_unit_frame() -> void:
	print("[unit_frame]")
	var frame = UnitFrame.new()
	root.add_child(frame)
	await _wait(1)  # let _ready() build the widgets

	# Half-health warrior with a RAGE pool.
	frame.render({
		"name": "Vanguard", "level": 10,
		"health": 500, "max_health": 1000,
		"power": 30, "max_power": 100, "power_type": PowerColors.RAGE,
	})
	await _wait(1)
	var health_fill: ColorRect = frame._health_fill
	_check("health bar ~50% width", absf(health_fill.size.x - UnitFrame.BAR_W * 0.5) < 1.0)
	_check("power bar visible for RAGE", frame._power_bar.visible)

	# A basic melee creature: PowerType NONE → no power bar.
	frame.render({
		"name": "Wolf", "level": 3,
		"health": 120, "max_health": 120,
		"power": 0, "max_power": 0, "power_type": PowerColors.NONE,
	})
	await _wait(1)
	_check("full health → full bar", absf(frame._health_fill.size.x - UnitFrame.BAR_W) < 1.0)
	_check("no power bar for NONE", not frame._power_bar.visible)
	frame.queue_free()


func _verify_hud_binding() -> void:
	print("[hud_binding]")
	var bus = EventBus.new()
	var hud = Hud.new()
	root.add_child(hud)
	hud.setup(bus)
	await _wait(1)

	# Identify the local player, then push a self VITALS_UPDATE — the player frame
	# must reflect it (bus → HUD → player frame).
	bus.seed_identity(0x100, "Aelric", 7, 1)
	bus.set_local_player(0x100)
	bus.publish_vitals_update(0x100, {
		"health": 640, "max_health": 800,
		"power": 50, "max_power": 100, "power_type": PowerColors.MANA, "level": 7,
	})
	await _wait(1)
	var pf: MeridianUnitFrame = hud._player_frame
	_check("player frame reflects self vitals",
		absf(pf._health_fill.size.x - UnitFrame.BAR_W * (640.0 / 800.0)) < 1.0)

	# Target frame: hidden with no target, shown + populated once a target is set.
	var tf: MeridianUnitFrame = hud._target_frame
	_check("target frame hidden initially", not tf.visible)
	bus.publish_entity_enter(0x2A, {
		"name": "Bot-7", "level": 5, "char_class": 3,
		"health": 300, "max_health": 1000,
		"power": 0, "max_power": 100, "power_type": PowerColors.ENERGY,
	})
	bus.set_target(0x2A)
	await _wait(1)
	_check("target frame shown on target", tf.visible)
	_check("target frame reflects target vitals",
		absf(tf._health_fill.size.x - UnitFrame.BAR_W * 0.3) < 1.0)
	hud.queue_free()


func _verify_decode_safety() -> void:
	print("[decode_safety]")
	# The new VITALS_UPDATE branch in decode_entity_frame must reject a garbage body
	# safely (kind "") — never crash. The real round-trip is proven in the C++ ctest.
	var net := MeridianNetThread.new()
	var d: Dictionary = net.decode_entity_frame(0x2004, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage VITALS_UPDATE → kind ''", String(d.get("kind", "x")) == "")
