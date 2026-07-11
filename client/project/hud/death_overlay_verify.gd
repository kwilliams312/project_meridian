# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the DEATH / GHOST / CORPSE-RUN /
# RESURRECT presentation (CMB-03, #359/#532): the event-bus death registry (phase machine +
# corpse-run auto-resurrect intent), the death overlay view, and the MeridianNetThread frame
# builders + decode safety. NOT a shipped scene: run it as
#   godot --headless --script res://hud/death_overlay_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the bus phase advances ONLY on server frames: publish_death_state → "dead" (emits died),
#     publish_ghost_state → "ghost" (emits became_ghost), publish_resurrect_result(OK) → "alive"
#     (emits resurrected + restores the local player's health); it never predicts a phase;
#   * the corpse-run completes as an INTENT: while a ghost, update_ghost_position fires
#     RESURRECT_REQUEST exactly once when the player reaches RESURRECT_RANGE_M (not before), and
#     a TOO_FAR refusal re-arms it for a retry;
#   * the death overlay opens on died (Release control + countdown), switches to the greyscale
#     ghost view on became_ghost (Resurrect control, enabled within range), and clears on a
#     RESURRECT_RESULT OK;
#   * MeridianNetThread builds the empty C→S RELEASE_REQUEST / RESURRECT_REQUEST frames non-empty
#     and its decode_death_frame rejects a garbage body safely (kind "") — the full wire round-trip
#     is proven by the C++ ctest (client/net/test — the death codec + golden cross-decode cases).
# Exits 0 on success, 1 on any failed assertion — same shape as combat_verify.gd.
extends SceneTree

const EventBus := preload("res://hud/event_bus.gd")
const DeathOverlay := preload("res://hud/death_overlay.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian DEATH / GHOST / RESURRECT RUNTIME verify (#532)")

	_verify_event_bus_flow()
	_verify_event_bus_refusal()
	await _verify_death_overlay_view()
	_verify_net_bridge()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


# --- Event-bus death registry: DEATH → GHOST → RESURRECT(OK) ------------------

func _verify_event_bus_flow() -> void:
	print("[event_bus]")
	var bus = EventBus.new()
	bus.set_local_player(0xAABBCCDD)
	# Seed a live-then-dead vitals baseline so the OK path can restore health onto it.
	bus.seed_identity(0xAABBCCDD, "Aldric", 5, 1)

	var died_events: Array = []
	var ghost_events: Array = []
	var rez_events: Array = []
	var intents: Array = []
	bus.died.connect(func(corpse, pos, ms): died_events.append([corpse, pos, ms]))
	bus.became_ghost.connect(func(gy, corpse_pos, corpse): ghost_events.append([gy, corpse_pos, corpse]))
	bus.resurrected.connect(func(status, hp, mhp): rez_events.append([status, hp, mhp]))
	bus.resurrect_requested.connect(func(): intents.append("resurrect"))

	_check("starts alive", bus.death_phase() == EventBus.DEATH_PHASE_ALIVE)

	# DEATH_STATE — corpse at (10, 0, -20), 6 s auto-release.
	var corpse_pos := Vector3(10.0, 0.0, -20.0)
	bus.publish_death_state(0xD0001, corpse_pos, 6000)
	_check("phase → dead", bus.death_phase() == EventBus.DEATH_PHASE_DEAD)
	_check("is_dead_or_ghost", bus.is_dead_or_ghost())
	_check("died emitted once", died_events.size() == 1)
	_check("died carries corpse guid", int(died_events[0][0]) == 0xD0001)
	_check("died carries auto-release ms", int(died_events[0][2]) == 6000)
	_check("corpse position stored", bus.corpse_position() == corpse_pos)

	# GHOST_STATE — released, ghost at the graveyard; corpse position carries over from death.
	var graveyard := Vector3(100.0, 0.0, 50.0)
	bus.publish_ghost_state(graveyard, 0xD0001)
	_check("phase → ghost", bus.death_phase() == EventBus.DEATH_PHASE_GHOST)
	_check("is_ghost", bus.is_ghost())
	_check("became_ghost emitted once", ghost_events.size() == 1)
	_check("ghost carries graveyard", ghost_events[0][0] == graveyard)
	_check("ghost carries the death corpse position", ghost_events[0][1] == corpse_pos)

	# Corpse-run: a FAR position does not resurrect; reaching range fires RESURRECT_REQUEST once.
	var far_fired: bool = bus.update_ghost_position(Vector3(80.0, 0.0, 40.0))
	_check("far from corpse does not resurrect", not far_fired and intents.size() == 0)
	var near_fired: bool = bus.update_ghost_position(Vector3(11.0, 0.0, -20.0))  # ~1 m from corpse
	_check("reaching the corpse fires RESURRECT_REQUEST", near_fired and intents.size() == 1)
	# A second tick within range must NOT re-send (the in-flight guard holds).
	bus.update_ghost_position(Vector3(10.5, 0.0, -20.0))
	_check("no duplicate RESURRECT_REQUEST while in flight", intents.size() == 1)

	# RESURRECT_RESULT OK — alive again, health restored onto the local player's vitals.
	bus.publish_resurrect_result(EventBus.RESURRECT_OK, 100, 200)
	_check("phase → alive", bus.death_phase() == EventBus.DEATH_PHASE_ALIVE)
	_check("resurrected emitted OK", rez_events.size() == 1 and int(rez_events[0][0]) == EventBus.RESURRECT_OK)
	var v := bus.local_vitals()
	_check("local health restored", int(v["health"]) == 100 and int(v["max_health"]) == 200)
	_check("corpse cleared on resurrect", bus.corpse_guid() == 0)


# --- Event-bus resurrect refusal (TOO_FAR re-arms the corpse-run) -------------

func _verify_event_bus_refusal() -> void:
	print("[event_bus refusal]")
	var bus = EventBus.new()
	bus.set_local_player(0xAABBCCDD)
	var intents: Array = []
	bus.resurrect_requested.connect(func(): intents.append("resurrect"))

	bus.publish_death_state(0xD0001, Vector3(0.0, 0.0, 0.0), 6000)
	bus.publish_ghost_state(Vector3(50.0, 0.0, 50.0), 0xD0001)
	# Reach the corpse → one intent.
	bus.update_ghost_position(Vector3(1.0, 0.0, 0.0))
	_check("first corpse-run fires one intent", intents.size() == 1)
	# Server refuses (TOO_FAR): still a ghost, guard re-armed.
	bus.publish_resurrect_result(EventBus.RESURRECT_TOO_FAR, 0, 200)
	_check("refusal keeps ghost phase", bus.death_phase() == EventBus.DEATH_PHASE_GHOST)
	# Next in-range tick may retry (a NEW intent).
	bus.update_ghost_position(Vector3(0.5, 0.0, 0.0))
	_check("refusal re-armed the corpse-run intent", intents.size() == 2)


# --- Death overlay view -------------------------------------------------------

func _verify_death_overlay_view() -> void:
	print("[death_overlay]")
	var bus = EventBus.new()
	bus.set_local_player(0xAABBCCDD)
	var overlay = DeathOverlay.new()
	get_root().add_child(overlay)
	await _wait(1)
	overlay.setup(bus)
	_check("overlay hidden at rest", not overlay.is_overlay_visible())

	# DEATH_STATE → the death overlay with the Release control + countdown.
	bus.publish_death_state(0xD0001, Vector3(10.0, 0.0, -20.0), 6000)
	await _wait(1)
	_check("overlay shown on death", overlay.is_overlay_visible())
	_check("title reads 'You have died'", overlay.title_text() == "You have died")
	_check("Release control shown", overlay.is_release_visible())
	_check("not a ghost view yet", not overlay.is_ghost_view())

	# GHOST_STATE → the greyscale ghost view; Resurrect disabled until in range.
	bus.publish_ghost_state(Vector3(100.0, 0.0, 50.0), 0xD0001)
	await _wait(1)
	_check("ghost greyscale view active", overlay.is_ghost_view())
	_check("title reads 'You are a ghost'", overlay.title_text() == "You are a ghost")
	_check("Release control hidden as a ghost", not overlay.is_release_visible())
	_check("Resurrect disabled out of range", not overlay.is_resurrect_enabled())

	# Corpse-run reaches range → the guidance says so and Resurrect enables.
	bus.update_ghost_position(Vector3(11.0, 0.0, -20.0))
	await _wait(1)
	_check("Resurrect enabled within corpse range", overlay.is_resurrect_enabled())

	# RESURRECT_RESULT OK → the overlay clears (normal view restored).
	bus.publish_resurrect_result(EventBus.RESURRECT_OK, 100, 200)
	await _wait(1)
	_check("overlay cleared on resurrect", not overlay.is_overlay_visible())
	_check("greyscale cleared on resurrect", not overlay.is_ghost_view())

	overlay.queue_free()
	await _wait(1)


# --- Net bridge (MeridianNetThread frame builders + decode safety) ------------

func _verify_net_bridge() -> void:
	print("[net_bridge]")
	var net := MeridianNetThread.new()
	_check("RELEASE_REQUEST frame non-empty", net.build_release_request_frame().size() > 0)
	_check("RESURRECT_REQUEST frame non-empty", net.build_resurrect_request_frame().size() > 0)

	# decode_death_frame rejects a garbage body safely (kind "") for a death op, and returns
	# kind "" for a non-death opcode.
	var bad_death: Dictionary = net.decode_death_frame(0x3010, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage DEATH_STATE → kind ''", String(bad_death.get("kind", "x")) == "")
	var bad_rez: Dictionary = net.decode_death_frame(0x3014, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage RESURRECT_RESULT → kind ''", String(bad_rez.get("kind", "x")) == "")
	var other: Dictionary = net.decode_death_frame(0x2001, PackedByteArray())
	_check("non-death opcode → kind ''", String(other.get("kind", "x")) == "")
