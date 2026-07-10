# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the ACTION BAR + ABILITY CAST + GCD
# sweep (CMB-01/UI-01, D-10, #432): the event-bus combat registry (optimistic GCD +
# rollback + cast bar), the action bar + cast bar views, and the MeridianNetThread cast
# frame builder + decode safety. NOT a shipped scene: run it as
#   godot --headless --script res://hud/combat_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the bus predicts the GCD OPTIMISTICALLY on request_cast + emits the CAST_REQUEST
#     intent, drops a press while a predicted GCD is live, and rolls the GCD back cleanly
#     (gcd_remaining_ms 0) / resyncs it (gcd_remaining_ms > 0) on CAST_FAILED (the D-10
#     path); the cast bar opens on a cast-time CAST_START and closes on CAST_RESULT /
#     CAST_FAILED;
#   * the action bar renders the greybox ability set as slots, a slot press issues the
#     intent + starts the sweep, and a CAST_FAILED flashes the typed reason;
#   * the cast bar shows only while a cast is in flight;
#   * MeridianNetThread builds the C→S CAST_REQUEST frame non-empty and its decode_cast_frame
#     rejects a garbage body safely (kind "") — the full wire round-trip is proven by the C++
#     ctest (client/net/test — the cast codec cases).
# Exits 0 on success, 1 on any failed assertion — same shape as econ_verify.gd.
extends SceneTree

const EventBus := preload("res://hud/event_bus.gd")
const AbilitySet := preload("res://hud/ability_set.gd")
const ActionBar := preload("res://hud/action_bar.gd")
const CastBar := preload("res://hud/cast_bar.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian ACTION BAR + ABILITY CAST + GCD RUNTIME verify (#432)")

	_verify_ability_set()
	_verify_known_abilities_wire()
	_verify_event_bus_gcd_rollback()
	_verify_event_bus_cast_bar()
	await _verify_action_bar()
	await _verify_cast_bar_view()
	_verify_net_bridge()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


# --- Greybox ability set ------------------------------------------------------

func _verify_ability_set() -> void:
	print("[ability_set]")
	var a := AbilitySet.greybox_abilities()
	_check("greybox set non-empty", a.size() >= 2)
	_check("slot 1 is the real Pickaxe Slam (id 2)", int(a[0]["ability_id"]) == 2)
	_check("slot 2 is the real Minor Healing (id 1)", int(a[1]["ability_id"]) == 1)
	_check("hotkeys are 1..N in order", int(a[0]["hotkey"]) == 1 and int(a[1]["hotkey"]) == 2)
	# The fixture now mirrors the wire metadata shape (#457): Pickaxe triggers the GCD,
	# Minor Healing is off-GCD, Firebolt is a cast-time spell.
	_check("Pickaxe Slam triggers the GCD", bool(a[0]["triggers_gcd"]))
	_check("Minor Healing is off-GCD", not bool(a[1]["triggers_gcd"]))
	_check("Firebolt is a cast-time spell (cast_ms>0)", int(a[2]["cast_ms"]) > 0)


# --- KNOWN_ABILITIES wire path (publish_known_abilities + the #456 GCD fix) ----

func _verify_known_abilities_wire() -> void:
	print("[known_abilities]")
	var bus = EventBus.new()
	var sets: Array = []
	bus.ability_set_changed.connect(func(a): sets.append(a))

	# A KNOWN_ABILITIES push (as decode_cast_frame emits it): an off-GCD instant strike +
	# a GCD cast-time spell. The bus normalizes the wire rows into canonical bar entries.
	bus.publish_known_abilities([
		{"ability_id": 2, "cast_ms": 0, "triggers_gcd": false, "resource_type": 0,
			"resource_cost": 0, "range_m": 5.0},
		{"ability_id": 1001, "cast_ms": 1500, "triggers_gcd": true, "resource_type": 1,
			"resource_cost": 20, "range_m": 30.0},
	])
	var bar := bus.abilities()
	_check("known-abilities seeded 2 slots", bar.size() == 2)
	_check("hotkeys derived from slot order", int(bar[0]["hotkey"]) == 1 and int(bar[1]["hotkey"]) == 2)
	_check("ability metadata carried (cast_ms/triggers_gcd)",
		int(bar[0]["cast_ms"]) == 0 and not bool(bar[0]["triggers_gcd"]) and
		int(bar[1]["cast_ms"]) == 1500 and bool(bar[1]["triggers_gcd"]))
	_check("ability_set_changed emitted on publish", sets.size() == 1)

	# THE #456 FIX: pressing the OFF-GCD ability (id 2) issues the cast but starts NO GCD —
	# it no longer over-predicts a 1.5 s cooldown for an ability the server doesn't gate.
	var intents: Array = []
	bus.cast_requested.connect(func(aid, _t, _ms): intents.append(aid))
	var fired_offgcd: bool = bus.request_cast(2, 1000)
	_check("off-GCD press fired", fired_offgcd)
	_check("off-GCD press started NO GCD", bus.gcd_duration_ms() == 0 and bus.gcd_remaining_ms(1000) == 0)
	# And it is NOT dropped even while a GCD from another ability is live.
	var fired_gcd: bool = bus.request_cast(1001, 1001)
	_check("GCD ability press started the GCD", fired_gcd and bus.gcd_remaining_ms(1001) == EventBus.GCD_MS)
	var fired_offgcd_during: bool = bus.request_cast(2, 1100)
	_check("off-GCD ability usable DURING a live GCD", fired_offgcd_during)
	_check("all three intents reached the wire", intents.size() == 3)

	# An EMPTY set (a freshly-created character knows nothing) clears the bar.
	bus.publish_known_abilities([])
	_check("empty KNOWN_ABILITIES clears the bar", bus.abilities().is_empty())


# --- Event bus: optimistic GCD + rollback / resync ----------------------------

func _verify_event_bus_gcd_rollback() -> void:
	print("[event_bus/gcd]")
	var bus = EventBus.new()
	bus.set_target(0xABCD)  # a current target so request_cast carries it

	var intents: Array = []
	bus.cast_requested.connect(func(aid, tgt, t): intents.append([aid, tgt, t]))
	var gcd_events: Array = []
	bus.gcd_changed.connect(func(s, d): gcd_events.append([s, d]))

	# A press at t=1000 predicts the GCD + emits the intent against the current target.
	var fired: bool = bus.request_cast(2, 1000)
	_check("request_cast fired", fired == true)
	_check("intent emitted (ability 2, target 0xABCD)", intents.size() == 1 and
		int(intents[0][0]) == 2 and int(intents[0][1]) == 0xABCD)
	_check("optimistic GCD started (duration GCD_MS)", int(gcd_events.back()[1]) == EventBus.GCD_MS)
	_check("gcd_remaining mid-window", bus.gcd_remaining_ms(1000) == EventBus.GCD_MS and
		bus.gcd_remaining_ms(1000 + 500) == EventBus.GCD_MS - 500)
	_check("gcd_remaining 0 after the window", bus.gcd_remaining_ms(1000 + EventBus.GCD_MS + 1) == 0)

	# A second press WHILE the predicted GCD is live is dropped (no wire spam).
	var fired2: bool = bus.request_cast(2, 1200)
	_check("press during GCD dropped", fired2 == false)
	_check("no second intent", intents.size() == 1)

	# CAST_FAILED with gcd_remaining_ms 0 → a CLEAN ROLLBACK (the GCD clears).
	var reasons: Array = []
	bus.cast_failed_reason.connect(func(aid, r): reasons.append([aid, r]))
	bus.publish_cast_failed(2, EventBus.CAST_FAIL_NO_TARGET, 0, 1300)
	_check("rollback cleared the GCD", bus.gcd_remaining_ms(1300) == 0 and bus.gcd_duration_ms() == 0)
	_check("failed reason surfaced", reasons.size() == 1 and int(reasons[0][1]) == EventBus.CAST_FAIL_NO_TARGET)
	# After a clean rollback a fresh press fires again immediately.
	_check("press fires again after rollback", bus.request_cast(2, 1400) == true)

	# CAST_FAILED with gcd_remaining_ms > 0 → RESYNC the clock to the server's remainder.
	bus.publish_cast_failed(2, EventBus.CAST_FAIL_ON_GCD, 800, 2000)
	_check("resync set GCD to server remainder", bus.gcd_remaining_ms(2000) == 800 and
		bus.gcd_remaining_ms(2000 + 800) == 0)


# --- Event bus: cast bar (CAST_START → CAST_RESULT / CAST_FAILED) --------------

func _verify_event_bus_cast_bar() -> void:
	print("[event_bus/cast_bar]")
	var bus = EventBus.new()
	var cast_events: Array = []
	bus.cast_bar_changed.connect(func(active, aid, s, d): cast_events.append([active, aid, s, d]))
	var results: Array = []
	bus.cast_result_received.connect(func(r): results.append(r))

	# An INSTANT ability (cast_ms 0) opens NO cast bar.
	bus.request_cast(2, 100)
	bus.publish_cast_start(2, 0, 5, 110)
	_check("instant CAST_START opens no cast bar", cast_events.is_empty() and not bus.cast_active())

	# A CAST_RESULT for the instant ability is surfaced (floating-text hook, #23).
	bus.publish_cast_result({"ability_id": 2, "outcome": EventBus.OUTCOME_CRIT, "amount": 42,
		"is_heal": false, "target_health": 8, "target_dead": false}, 120)
	_check("CAST_RESULT surfaced", results.size() == 1 and int(results[0]["amount"]) == 42)

	# A CAST-TIME ability (cast_ms > 0) opens the cast bar; CAST_RESULT closes it.
	var bus2 = EventBus.new()
	var ev2: Array = []
	bus2.cast_bar_changed.connect(func(active, aid, s, d): ev2.append([active, aid, s, d]))
	bus2.request_cast(1001, 1000)
	bus2.publish_cast_start(1001, 1500, 9, 1010)
	_check("cast-time CAST_START opens the cast bar", bus2.cast_active() and
		bus2.cast_duration_ms() == 1500 and ev2.size() == 1 and bool(ev2[0][0]) == true)
	bus2.publish_cast_result({"ability_id": 1001}, 2500)
	_check("CAST_RESULT closes the cast bar", not bus2.cast_active() and
		ev2.size() == 2 and bool(ev2[1][0]) == false)

	# A CAST_FAILED / INTERRUPTED mid-cast closes the cast bar too.
	var bus3 = EventBus.new()
	bus3.request_cast(1001, 3000)
	bus3.publish_cast_start(1001, 1500, 9, 3010)
	_check("cast active before interrupt", bus3.cast_active())
	bus3.publish_cast_failed(1001, EventBus.CAST_FAIL_INTERRUPTED, 0, 3200)
	_check("interrupt closes the cast bar", not bus3.cast_active())


# --- Action bar view ----------------------------------------------------------

func _verify_action_bar() -> void:
	print("[action_bar]")
	var bus = EventBus.new()
	bus.set_target(0xFEED)
	var bar = ActionBar.new()
	get_root().add_child(bar)
	await _wait(1)
	bar.setup(bus)
	bus.seed_abilities(AbilitySet.greybox_abilities())
	await _wait(1)

	var slots := _count_slot_buttons(bar)
	_check("action bar built a slot per ability", slots == AbilitySet.greybox_abilities().size())

	# A slot press issues the CAST_REQUEST intent for that slot's ability against the target.
	var intents: Array = []
	bus.cast_requested.connect(func(aid, tgt, t): intents.append([aid, tgt]))
	bar.press_slot(0)  # slot 0 = Pickaxe Slam (id 2)
	await _wait(1)
	_check("slot press issued the intent", intents.size() == 1 and int(intents[0][0]) == 2 and
		int(intents[0][1]) == 0xFEED)
	_check("press started the predicted GCD", bus.gcd_remaining_ms(Time.get_ticks_msec()) > 0)

	# A CAST_FAILED flashes the typed reason (view stays alive / no crash).
	bus.publish_cast_failed(2, EventBus.CAST_FAIL_OUT_OF_RANGE, 0, Time.get_ticks_msec())
	await _wait(1)
	_check("action bar survived a CAST_FAILED flash", is_instance_valid(bar))

	bar.queue_free()
	await _wait(1)


# --- Cast bar view ------------------------------------------------------------

func _verify_cast_bar_view() -> void:
	print("[cast_bar]")
	var bus = EventBus.new()
	var bar = CastBar.new()
	get_root().add_child(bar)
	await _wait(1)
	bar.setup(bus)
	_check("cast bar hidden at rest", not bar.visible)

	# A cast-time CAST_START shows the bar; CAST_RESULT hides it.
	bus.request_cast(1001, Time.get_ticks_msec())
	bus.publish_cast_start(1001, 1500, 0, Time.get_ticks_msec())
	await _wait(1)
	_check("cast bar shown during a cast", bar.visible)
	bus.publish_cast_result({"ability_id": 1001}, Time.get_ticks_msec())
	await _wait(1)
	_check("cast bar hidden after CAST_RESULT", not bar.visible)

	bar.queue_free()
	await _wait(1)


# --- Net bridge (MeridianNetThread cast frame builder + decode safety) --------

func _verify_net_bridge() -> void:
	print("[net_bridge]")
	var net := MeridianNetThread.new()
	_check("cast_request frame non-empty", net.build_cast_request_frame(2, 0xBEEF, 1234).size() > 0)

	# decode_cast_frame rejects a garbage body safely (kind "") for a combat op, and returns
	# kind "" for a non-combat opcode.
	var bad: Dictionary = net.decode_cast_frame(0x3002, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage CAST_START → kind ''", String(bad.get("kind", "x")) == "")
	# KNOWN_ABILITIES (0x3005) rides the same combat decode seam; a garbage body is safe.
	var bad_ka: Dictionary = net.decode_cast_frame(0x3005, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage KNOWN_ABILITIES → kind ''", String(bad_ka.get("kind", "x")) == "")
	var other: Dictionary = net.decode_cast_frame(0x2001, PackedByteArray())
	_check("non-combat opcode → kind ''", String(other.get("kind", "x")) == "")


# --- helpers -----------------------------------------------------------------

# Count the transparent click-target Buttons the action bar builds (one per slot).
func _count_slot_buttons(node: Node) -> int:
	var n := 0
	if node is Button:
		n += 1
	for c in node.get_children():
		n += _count_slot_buttons(c)
	return n
