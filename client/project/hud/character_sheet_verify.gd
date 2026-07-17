# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the CHARACTER SHEET window
# (#870, epic #866): the paperdoll + equipment slots + click-to-equip/unequip UI over the
# equipment backend #802 already shipped. NOT a shipped scene: run it as
#   godot --headless --script res://hud/character_sheet_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the bus carries the local body seed (race/sex/appearance) the paperdoll needs, and
#     emits local_appearance_changed;
#   * the window opens/closes on toggle();
#   * every paperdoll POSITION renders, occupied rows showing the item and empty rows
#     reading "empty" — driven purely by equipment_items();
#   * clicking Equip on a backpack row emits request_equip(backpack_slot) and Unequip on an
#     occupied slot emits request_unequip(equipment_slot) — the exact intents the world
#     scene forwards to worldd;
#   * a TYPED reject (wrong class / level / slot) surfaces user-visible feedback and does
#     NOT move any displayed state (server is law — no client prediction);
#   * a re-pushed authoritative snapshot REFRESHES the sheet (equip lands, slot empties);
#   * the paperdoll mounts a body from the seed and wears the authoritative equipment.
# Exits 0 on success, 1 on any failed assertion — same shape as econ_verify.gd.
extends SceneTree

# Preloaded BY PATH (never the bare class name): a --script run has no autoloads and a
# freshly-added class_name is invisible to a stale global class cache.
const EventBus := preload("res://hud/event_bus.gd")
const CharacterSheetWindow := preload("res://hud/character_sheet_window.gd")

# The number of assertions this suite is EXPECTED to run. The exit code alone is a liar: a
# suite that fails to load (or silently no-ops) asserts nothing and still exits 0, which reads
# as green (this session has been bitten by exactly that). The guard in _initialize() fails
# LOUDLY if fewer than this many checks actually ran — so a fail-to-load can never pass. Bump
# this when adding checks; it only has to be a floor, not the exact count.
const MIN_EXPECTED_CHECKS := 60

var _fails := 0
var _checks := 0  # total assertions actually executed (the check-count guard's evidence)


func _check(name: String, ok: bool) -> void:
	_checks += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian CHARACTER SHEET UI RUNTIME verify (#870/#866)")

	_verify_event_bus_appearance_seed()
	await _verify_window_toggle()
	await _verify_slot_rendering()
	await _verify_equip_intents()
	await _verify_reject_feedback()
	await _verify_refresh_on_snapshot()
	await _verify_paperdoll()
	await _verify_stats_panel()

	# ⛔ CHECK-COUNT GUARD (never trust the exit code): if the suite asserted fewer than the
	# expected floor, something failed to LOAD or run — treat that as a hard failure even
	# though every check that DID run may have passed. This is what keeps a fail-to-load from
	# masquerading as green.
	print("\n%d check(s) ran (floor %d), %d failure(s)" % [_checks, MIN_EXPECTED_CHECKS, _fails])
	if _checks < MIN_EXPECTED_CHECKS:
		print("  [FAIL] check-count guard: only %d of >=%d expected checks ran — the suite did "
			% [_checks, MIN_EXPECTED_CHECKS]
			+ "not fully execute (fail-to-load / no-op). Failing loudly.")
		quit(1)
		return
	quit(1 if _fails > 0 else 0)


# --- Event bus: the local body seed ------------------------------------------

func _verify_event_bus_appearance_seed() -> void:
	print("[event_bus/local_appearance]")
	var bus = EventBus.new()
	_check("appearance unknown before seed", not bus.local_appearance_known())

	var events: Array = []
	bus.local_appearance_changed.connect(func(r, s, a): events.append([r, s, a]))
	bus.seed_local_appearance(2, 1, {"skin": 3, "hair": 1})
	_check("local_appearance_changed emitted", events.size() == 1)
	_check("appearance known after seed", bus.local_appearance_known())
	_check("race stored", bus.local_race() == 2)
	_check("sex stored", bus.local_sex() == 1)
	_check("appearance record stored", int(bus.local_appearance().get("skin", 0)) == 3)

	# The accessor hands out a COPY — a view mutating it must not corrupt bus state.
	var copy := bus.local_appearance()
	copy["skin"] = 99
	_check("local_appearance() returns a copy",
		int(bus.local_appearance().get("skin", 0)) == 3)


# --- Window: open / close -----------------------------------------------------

func _verify_window_toggle() -> void:
	print("[character_sheet/toggle]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	_check("sheet hidden by default", not win.visible)
	win.toggle()
	await _wait(1)
	_check("sheet visible on toggle", win.visible)
	_check("sheet waits for the snapshot before any state",
		_find_label_containing(win, "Waiting for equipment snapshot") != null)
	win.toggle()
	await _wait(1)
	_check("sheet hidden on second toggle", not win.visible)

	# The Close button is the mouse path to the same state.
	win.toggle()
	await _wait(1)
	var close := _find_button_containing(win, "Close")
	_check("sheet has a Close button labelled with its keybind", close != null and
		close.text.find("[C]") != -1)
	close.emit_signal("pressed")
	await _wait(1)
	_check("Close button hides the sheet", not win.visible)

	win.queue_free()


# --- Window: the equipment slots ---------------------------------------------

func _verify_slot_rendering() -> void:
	print("[character_sheet/slots]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	win.toggle()
	await _wait(1)

	# An authoritative snapshot: chest (rare), neck (uncommon, bound), main hand.
	bus.publish_inventory_snapshot(0, [], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900012,
			"quality": 3, "binding": 0},
		{"slot": CharacterSheetWindow.SLOT_NECK, "item_template_id": 900021,
			"quality": 2, "binding": 1},
		{"slot": CharacterSheetWindow.SLOT_MAIN_HAND, "item_template_id": 900033,
			"quality": 4, "binding": 0},
	])
	await _wait(1)

	# Every paperdoll position from the #866 data contract is present (head…feet, the
	# jewellery slots, weapons) — occupied or not.
	for label in ["Head", "Shoulders", "Back", "Chest", "Wrist", "Hands", "Waist", "Legs",
			"Feet", "Neck", "Finger", "Trinket", "Main Hand", "Off Hand", "Ranged"]:
		_check("slot row present: %s" % label,
			_find_label_containing(win, label) != null)

	_check("occupied chest renders its item", _find_label_containing(win, "Item #900012") != null)
	_check("occupied neck renders its item", _find_label_containing(win, "Item #900021") != null)
	_check("occupied main hand renders its item",
		_find_label_containing(win, "Item #900033") != null)
	_check("a bound item is marked bound",
		_find_label_containing(win, "Item #900021  (bound)") != null)

	# 15 positions, 3 filled → 12 empty markers.
	_check("empty slots read as empty (12 of 15)",
		_count_labels_containing(win, "— empty —") == 12)
	# Only occupied slots offer Unequip.
	_check("only occupied slots offer Unequip (3)",
		_count_buttons_containing(win, "Unequip") == 3)

	# Quality drives the row colour, the same palette bags/loot use (epic = purple).
	var epic_label := _find_label_containing(win, "Item #900033")
	_check("epic item uses the shared rarity colour", epic_label != null and
		epic_label.get_theme_color("font_color").is_equal_approx(Color(0.64, 0.21, 0.93)))

	win.queue_free()


# --- Window: click-to-equip / click-to-unequip intents ------------------------

func _verify_equip_intents() -> void:
	print("[character_sheet/intents]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	win.toggle()
	await _wait(1)

	var intents: Array = []
	bus.equipment_change_requested.connect(func(a, s): intents.append([a, s]))

	bus.publish_inventory_snapshot(0, [
		{"slot": 100, "item_template_id": 900007, "count": 1, "quality": 1, "binding": 0},
	], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900012,
			"quality": 3, "binding": 0},
	])
	await _wait(1)

	# Equip from a BACKPACK row → request_equip(backpack_slot). action 0 = equip.
	var equip := _find_button_containing(win, "Equip")
	_check("backpack row offers a keyboard-focusable Equip button", equip != null and
		equip.focus_mode != Control.FOCUS_NONE)
	_check("Equip exposes an item-specific accessible name", equip != null and
		"900007" in equip.accessibility_name)
	equip.emit_signal("pressed")
	_check("Equip emits request_equip on the backpack slot", intents.size() == 1 and
		int(intents[0][0]) == 0 and int(intents[0][1]) == 100)

	# One in flight at a time: buttons disable until the server answers.
	await _wait(1)
	_check("in-flight change disables further Equip",
		_find_button_containing(win, "Equip").disabled)
	_check("in-flight change reports progress",
		_find_label_containing(win, "Updating equipment") != null)
	var second := _find_button_containing(win, "Unequip")
	if second != null:
		second.emit_signal("pressed")
	_check("a second click while in flight emits nothing", intents.size() == 1)

	# Server answered → the sheet is live again; Unequip → request_unequip(equip_slot).
	bus.publish_inventory_snapshot(0, [
		{"slot": 100, "item_template_id": 900007, "count": 1, "quality": 1, "binding": 0},
	], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900012,
			"quality": 3, "binding": 0},
	])
	await _wait(1)
	var unequip := _find_button_containing(win, "Unequip")
	_check("Unequip exposes a slot-specific accessible name", unequip != null and
		"Chest" in unequip.accessibility_name)
	unequip.emit_signal("pressed")
	_check("Unequip emits request_unequip on the equipment slot", intents.size() == 2 and
		int(intents[1][0]) == 1 and int(intents[1][1]) == CharacterSheetWindow.SLOT_CHEST)

	win.queue_free()


# --- Window: typed rejects surface as feedback --------------------------------

func _verify_reject_feedback() -> void:
	print("[character_sheet/rejects]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	win.toggle()
	await _wait(1)

	bus.publish_inventory_snapshot(0, [
		{"slot": 100, "item_template_id": 900007, "count": 1, "quality": 1, "binding": 0},
	], 16, [])
	await _wait(1)

	# The three rejects the story calls out, each a distinct player-readable sentence.
	bus.publish_equipment_change_result({"status": 6, "action": 0, "slot": 100})
	_check("wrong-class reject is actionable",
		_find_label_containing(win, "not proficient") != null)
	bus.publish_equipment_change_result({"status": 5, "action": 0, "slot": 100})
	_check("level reject is actionable",
		_find_label_containing(win, "level is too low") != null)
	bus.publish_equipment_change_result({"status": 7, "action": 0, "slot": 100})
	_check("slot-mismatch reject is actionable",
		_find_label_containing(win, "does not match") != null)
	bus.publish_equipment_change_result({"status": 99, "action": 0, "slot": 100})
	_check("an unknown status still surfaces feedback",
		_find_label_containing(win, "rejected") != null)

	# SERVER IS LAW: a reject moved nothing, so the paperdoll must be untouched and the
	# sheet must be usable again (no stuck in-flight lock). Wait a frame first — a re-render
	# queue_free()s the old rows, which only leave the tree at the end of the frame.
	await _wait(1)
	_check("reject left every slot empty (no client prediction)",
		_count_labels_containing(win, "— empty —") == 15)
	_check("reject re-enables the Equip button",
		not _find_button_containing(win, "Equip").disabled)

	win.queue_free()


# --- Window: refresh on the authoritative snapshot ----------------------------

func _verify_refresh_on_snapshot() -> void:
	print("[character_sheet/refresh]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	win.toggle()
	await _wait(1)

	bus.publish_inventory_snapshot(0, [
		{"slot": 100, "item_template_id": 900007, "count": 1, "quality": 1, "binding": 0},
	], 16, [])
	await _wait(1)
	_check("before equip: item is in the backpack",
		_find_label_containing(win, "[100] Item #900007") != null)
	_check("before equip: all 15 slots empty",
		_count_labels_containing(win, "— empty —") == 15)

	# The server accepted the equip and re-pushed: the item left the bags and landed in the
	# chest slot. The sheet must follow the snapshot, not a prediction.
	bus.publish_inventory_snapshot(0, [], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900007,
			"quality": 1, "binding": 1},
	])
	await _wait(1)
	_check("equipment_changed refreshed the chest slot",
		_find_label_containing(win, "Item #900007  (bound)") != null)
	_check("after equip: 14 slots empty",
		_count_labels_containing(win, "— empty —") == 14)
	_check("after equip: bags read empty",
		_find_label_containing(win, "bags are empty") != null)

	# And the reverse — an unequip snapshot empties the slot again.
	bus.publish_inventory_snapshot(0, [
		{"slot": 100, "item_template_id": 900007, "count": 1, "quality": 1, "binding": 1},
	], 16, [])
	await _wait(1)
	_check("unequip snapshot emptied the chest slot again",
		_count_labels_containing(win, "— empty —") == 15)

	# A sheet opened AFTER the snapshot landed is never stale.
	win.toggle()
	await _wait(1)
	win.toggle()
	await _wait(1)
	_check("re-opened sheet renders the held state",
		_find_label_containing(win, "[100] Item #900007") != null)

	win.queue_free()


# --- Window: the paperdoll preview -------------------------------------------

func _verify_paperdoll() -> void:
	print("[character_sheet/paperdoll]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	win.toggle()
	await _wait(2)

	# The REUSED char-select widget (not a fork) is mounted.
	var paperdoll: Node = win.find_child("Paperdoll", true, false)
	_check("the char-select paperdoll widget is mounted", paperdoll != null)
	_check("paperdoll is the reused SubViewportContainer widget",
		paperdoll is SubViewportContainer)

	# Seeding the local body mounts a model (a real assembled body when content is present,
	# the widget's capsule fallback when it is not — either way, "PreviewBody").
	bus.seed_local_appearance(0, 0, MeridianAppearance.default_appearance())
	await _wait(2)
	var body: Node = win.find_child("PreviewBody", true, false)
	_check("seeding the local appearance mounts a preview body", body != null)

	# The paperdoll wears the AUTHORITATIVE equipment, remapped to the assembler's key
	# (`item_template_id` on the wire → `item_template` for assemble()).
	bus.publish_inventory_snapshot(0, [], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900012,
			"quality": 3, "binding": 0},
	])
	await _wait(2)
	_check("paperdoll survives an equipment refresh",
		win.find_child("PreviewBody", true, false) != null)
	var mapped: Array = win._paperdoll_equipment()
	_check("equipment is remapped to the assembler's contract", mapped.size() == 1 and
		int((mapped[0] as Dictionary).get("item_template", 0)) == 900012 and
		int((mapped[0] as Dictionary).get("slot", -1)) == CharacterSheetWindow.SLOT_CHEST)
	_check("the worn set the paperdoll shows is tracked", win._rendered_equipment == mapped)

	# A snapshot that does NOT touch equipment (a loot/vendor/quest re-push — by far the
	# common case) must not rebuild the 3D body: same worn set in, same mounted body out.
	var body_before: Node = win.find_child("PreviewBody", true, false)
	bus.publish_inventory_snapshot(500, [
		{"slot": 100, "item_template_id": 900007, "count": 1, "quality": 1, "binding": 0},
	], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900012,
			"quality": 3, "binding": 0},
	])
	await _wait(2)
	_check("an equipment-identical snapshot does not re-assemble the body",
		win.find_child("PreviewBody", true, false) == body_before)
	_check("the backpack still refreshed on that snapshot",
		_find_label_containing(win, "[100] Item #900007") != null)

	# But a snapshot that DOES change the worn set must re-assemble.
	bus.publish_inventory_snapshot(500, [], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900099,
			"quality": 3, "binding": 0},
	])
	await _wait(2)
	_check("a changed worn set re-assembles the body",
		win.find_child("PreviewBody", true, false) != body_before)
	_check("the changed worn set reached the slot row",
		_find_label_containing(win, "Item #900099") != null)

	win.queue_free()


# --- Window: the stats sub-panel (#898) --------------------------------------

func _verify_stats_panel() -> void:
	print("[character_sheet/stats]")
	var bus = EventBus.new()
	var win = CharacterSheetWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	win.toggle()
	await _wait(1)

	# The panel HEADER always exists (the section is a fixed part of the sheet).
	_check("stats section header present", _find_label_containing(win, "Stats") != null)

	# NO-SHEET CASE — a content-less realm never pushes CHARACTER_STATS. The panel must show a
	# graceful absent state, not a broken/half-rendered one, and never claim stats it lacks.
	_check("stats unknown before any sheet", not bus.stats_known())
	_check("absent state shown when no sheet arrived",
		_find_label_containing(win, "not available") != null)
	_check("no stat value is fabricated with no sheet",
		_find_label_exact(win, "Gear Armor") == null)

	# A CHARACTER_STATS lands (level 8, agility 14 + strength 27, gear armor 165). The panel
	# renders a row per effective attribute (ref humanized), a Level row, and a DISTINCT Gear
	# Armor row — every value read straight off the bus projection.
	var stats_events: Array = []
	bus.stats_changed.connect(func(lvl, attrs, ga): stats_events.append([lvl, attrs, ga]))
	bus.publish_character_stats(8, [
		{"ref": "core:attribute.agility", "value": 14},
		{"ref": "core:attribute.strength", "value": 27},
	], 165)
	await _wait(1)
	_check("stats_changed emitted", stats_events.size() == 1)
	_check("stats known after a sheet", bus.stats_known())
	_check("absent state replaced once a sheet lands",
		_find_label_containing(win, "not available") == null)
	_check("level row rendered", _find_label_exact(win, "Level") != null and
		_find_label_exact(win, "8") != null)
	_check("strength attribute humanized + valued",
		_find_label_exact(win, "Strength") != null and _find_label_exact(win, "27") != null)
	_check("agility attribute humanized + valued",
		_find_label_exact(win, "Agility") != null and _find_label_exact(win, "14") != null)
	_check("gear armor shown as its own distinct row",
		_find_label_exact(win, "Gear Armor") != null and _find_label_exact(win, "165") != null)

	# UPDATE ON EQUIP — an equip-triggered recompute pushes a fresh sheet; the panel follows it
	# (server is law, never predicted). Strength 27 -> 31, gear armor 165 -> 190.
	bus.publish_character_stats(8, [
		{"ref": "core:attribute.agility", "value": 14},
		{"ref": "core:attribute.strength", "value": 31},
	], 190)
	await _wait(1)
	_check("a fresh sheet updates the changed attribute",
		_find_label_exact(win, "31") != null and _find_label_exact(win, "27") == null)
	_check("a fresh sheet updates gear armor",
		_find_label_exact(win, "190") != null and _find_label_exact(win, "165") == null)

	# The stats projection SURVIVES an equipment/inventory snapshot re-render (the sheet is not
	# wiped when only bags/paperdoll change) — a _render_all() must keep showing the held stats.
	bus.publish_inventory_snapshot(0, [], 16, [
		{"slot": CharacterSheetWindow.SLOT_CHEST, "item_template_id": 900044,
			"quality": 3, "binding": 0},
	])
	await _wait(1)
	_check("stats survive an equipment snapshot re-render",
		_find_label_exact(win, "Strength") != null and _find_label_exact(win, "31") != null)

	# The accessor hands out a COPY — a view mutating it must not corrupt bus state.
	var copy := bus.character_stats()
	if not copy.is_empty():
		(copy[0] as Dictionary)["value"] = 999
	_check("character_stats() returns a copy",
		int((bus.character_stats()[0] as Dictionary).get("value", 0)) != 999)

	win.queue_free()


# --- helpers -----------------------------------------------------------------

func _find_label_exact(root_node: Node, text: String) -> Label:
	for n in _walk(root_node):
		if n is Label and (n as Label).text == text:
			return n
	return null


func _find_button_containing(root_node: Node, needle: String) -> Button:
	for n in _walk(root_node):
		if n is Button and (n as Button).text.find(needle) != -1:
			return n
	return null


func _count_buttons_containing(root_node: Node, needle: String) -> int:
	var count := 0
	for n in _walk(root_node):
		if n is Button and (n as Button).text.find(needle) != -1:
			count += 1
	return count


func _find_label_containing(root_node: Node, needle: String) -> Label:
	for n in _walk(root_node):
		if n is Label and (n as Label).text.find(needle) != -1:
			return n
	return null


func _count_labels_containing(root_node: Node, needle: String) -> int:
	var count := 0
	for n in _walk(root_node):
		if n is Label and (n as Label).text.find(needle) != -1:
			count += 1
	return count


func _walk(n: Node) -> Array:
	var out: Array = [n]
	for c in n.get_children():
		out.append_array(_walk(c))
	return out
