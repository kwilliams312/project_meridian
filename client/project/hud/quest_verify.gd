# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the QUEST + GOSSIP UI (QST-01,
# #433): the event-bus quest/gossip registry, the gossip window, the quest log window,
# the quest tracker, and the MeridianNetThread quest/gossip frame builders + decode
# safety. NOT a shipped scene: run it as
#   godot --headless --script res://hud/quest_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the bus stores + re-emits the server's quest/gossip state (GOSSIP_MENU → giver
#     marker; QUEST_LOG snapshot → tracked-quest pick; QUEST_PROGRESS merge; accept /
#     turn-in results drive tracking + log removal);
#   * the three windows render that state (options, quest blocks, tracker lines) and
#     issue the right INTENTS back through the bus (accept / turn-in / vendor / trainer);
#   * MeridianNetThread builds the C→S quest/gossip frames non-empty and its
#     decode_quest_frame rejects a garbage body safely (kind "") — the full wire
#     round-trip is proven by the C++ ctest (client/net/test — quest/gossip codec cases).
# Exits 0 on success, 1 on any failed assertion — same shape as hud_verify.gd.
extends SceneTree

const EventBus := preload("res://hud/event_bus.gd")
const GossipWindow := preload("res://hud/gossip_window.gd")
const QuestLogWindow := preload("res://hud/quest_log_window.gd")
const QuestTracker := preload("res://hud/quest_tracker.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian QUEST + GOSSIP UI RUNTIME verify (#433)")

	_verify_event_bus_quest()
	_verify_event_bus_gossip()
	await _verify_gossip_window()
	await _verify_quest_log_window()
	await _verify_quest_tracker()
	_verify_net_bridge()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


# A populated two-quest log snapshot (the shape decode_quest_frame produces).
func _sample_log() -> Array:
	return [
		{
			"quest_id": 28, "level": 4, "complete": false,
			"objectives": [
				{"type": 0, "target_id": 26, "have": 3, "need": 8, "complete": false},
			],
		},
		{
			"quest_id": 29, "level": 5, "complete": true,
			"objectives": [
				{"type": 1, "target_id": 50, "have": 6, "need": 6, "complete": true},
			],
		},
	]


func _verify_event_bus_quest() -> void:
	print("[event_bus/quest]")
	var bus = EventBus.new()

	# QUEST_LOG snapshot lands, is readable, and auto-tracks the FIRST active quest.
	var log_events: Array = []
	bus.quest_log_changed.connect(func(q): log_events.append(q))
	var track_events: Array = []
	bus.tracked_quest_changed.connect(func(qid): track_events.append(qid))
	bus.publish_quest_log(_sample_log())
	_check("log stored (2 quests)", bus.quest_log().size() == 2)
	_check("has_quest(28)", bus.has_quest(28))
	_check("auto-tracked first quest (28)", bus.tracked_quest() == 28)
	_check("quest_log_changed emitted", log_events.size() == 1)
	_check("tracked_quest_changed emitted", track_events.size() >= 1 and int(track_events[-1]) == 28)

	# QUEST_PROGRESS merges onto the tracked entry's objective.
	var prog_events: Array = []
	bus.quest_progress_changed.connect(func(qid, i, h, n, c): prog_events.append([qid, i, h, n, c]))
	bus.publish_quest_progress({
		"quest_id": 28, "objective_index": 0, "type": 0, "have": 7, "need": 8, "complete": false,
	})
	var entry: Dictionary = bus.tracked_quest_entry()
	_check("progress merged have=7", int((entry["objectives"][0] as Dictionary)["have"]) == 7)
	_check("progress signal fired", prog_events.size() == 1 and int(prog_events[0][2]) == 7)

	# A completing delta flips the entry to turn-in-ready (all objectives complete).
	bus.publish_quest_progress({
		"quest_id": 28, "objective_index": 0, "type": 0, "have": 8, "need": 8, "complete": true,
	})
	_check("entry complete when all objectives done", bool(bus.tracked_quest_entry()["complete"]))

	# ACCEPT result OK tracks the newly accepted quest.
	bus.publish_quest_accept_result(29, 0)  # OK
	_check("accept OK tracks quest 29", bus.tracked_quest() == 29)

	# A gated accept (LEVEL_TOO_LOW=4) does NOT change tracking + surfaces the status.
	var accept_status := {"v": -1}
	bus.quest_accept_result.connect(func(_q, s): accept_status["v"] = s)
	bus.publish_quest_accept_result(70, 4)
	_check("gated accept surfaces status 4", int(accept_status["v"]) == 4)
	_check("gated accept keeps tracking 29", bus.tracked_quest() == 29)

	# TURN_IN OK drops the quest from the log + re-picks a tracked quest.
	var ti_events: Array = []
	bus.quest_turn_in_result.connect(func(r): ti_events.append(r))
	bus.publish_quest_turn_in_result({
		"quest_id": 29, "status": 0, "reward_xp": 420, "reward_money": 400,
		"new_level": 6, "reward_items": [{"item_id": 101, "count": 1}],
	})
	_check("turn-in OK removed quest 29", not bus.has_quest(29))
	_check("turn-in re-tracked remaining quest 28", bus.tracked_quest() == 28)
	_check("turn_in_result carried rewards", ti_events.size() == 1 and int((ti_events[0] as Dictionary)["reward_xp"]) == 420)


func _verify_event_bus_gossip() -> void:
	print("[event_bus/gossip]")
	var bus = EventBus.new()

	# A GOSSIP_MENU with a turn-in-ready quest yields a "?" giver marker (outranks "!").
	var marker := {"v": "x"}
	bus.giver_indicator_changed.connect(func(_g, m): marker["v"] = m)
	var menu_events: Array = []
	bus.gossip_menu_changed.connect(func(g, o): menu_events.append([g, o]))
	bus.publish_gossip_menu(27, [
		{"kind": EventBus.GOSSIP_QUEST_AVAILABLE, "target_id": 28},
		{"kind": EventBus.GOSSIP_QUEST_COMPLETE, "target_id": 29},
		{"kind": EventBus.GOSSIP_VENDOR, "target_id": 0},
	])
	_check("gossip_menu_changed fired for npc 27", menu_events.size() == 1 and int(menu_events[0][0]) == 27)
	_check("gossip menu stored (3 options)", bus.gossip_options().size() == 3)
	_check("gossip npc is 27", bus.gossip_npc() == 27)
	_check("giver marker '?' (turn-in outranks available)", String(marker["v"]) == "?")
	_check("giver_marker getter agrees", bus.giver_marker(27) == "?")

	# An available-only menu yields "!".
	bus.publish_gossip_menu(62, [{"kind": EventBus.GOSSIP_QUEST_AVAILABLE, "target_id": 71}])
	_check("available-only marker is '!'", bus.giver_marker(62) == "!")

	# close_gossip clears the open menu + fires gossip_closed.
	var closed := {"v": false}
	bus.gossip_closed.connect(func(): closed["v"] = true)
	bus.close_gossip()
	_check("gossip closed clears npc", bus.gossip_npc() == 0)
	_check("gossip_closed emitted", bool(closed["v"]))

	# Vendor / trainer entry hooks re-emit for the sibling #440 windows.
	var vendor := {"v": 0}
	var trainer := {"v": 0}
	bus.vendor_entry_selected.connect(func(g): vendor["v"] = g)
	bus.trainer_entry_selected.connect(func(g): trainer["v"] = g)
	bus.request_vendor_entry(27)
	bus.request_trainer_entry(64)
	_check("vendor entry hook fires", int(vendor["v"]) == 27)
	_check("trainer entry hook fires", int(trainer["v"]) == 64)


func _verify_gossip_window() -> void:
	print("[gossip_window]")
	var bus = EventBus.new()
	var win = GossipWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	# An intent is emitted when a quest-accept row is pressed.
	var accept := {"quest": 0, "giver": 0}
	bus.quest_accept_requested.connect(func(q, g): accept["quest"] = q; accept["giver"] = g)

	bus.publish_gossip_menu(27, [
		{"kind": EventBus.GOSSIP_QUEST_AVAILABLE, "target_id": 28},
		{"kind": EventBus.GOSSIP_QUEST_IN_PROGRESS, "target_id": 40},
		{"kind": EventBus.GOSSIP_QUEST_COMPLETE, "target_id": 29},
		{"kind": EventBus.GOSSIP_VENDOR, "target_id": 0},
		{"kind": EventBus.GOSSIP_TRAINER, "target_id": 0},
	])
	await _wait(1)
	_check("gossip window visible on menu", win.visible)
	# Find the accept button (an available-quest row) and press it.
	var accept_btn := _find_button_containing(win, "Accept quest #28")
	_check("accept-quest button rendered", accept_btn != null)
	if accept_btn != null:
		accept_btn.emit_signal("pressed")
	_check("pressing accept emits intent (quest 28, giver 27)",
		int(accept["quest"]) == 28 and int(accept["giver"]) == 27)

	# Turn-in row emits a turn-in intent.
	var turn := {"quest": 0}
	bus.quest_turn_in_requested.connect(func(q, _n, _c): turn["quest"] = q)
	var turn_btn := _find_button_containing(win, "Turn in quest #29")
	_check("turn-in button rendered", turn_btn != null)
	if turn_btn != null:
		turn_btn.emit_signal("pressed")
	_check("pressing turn-in emits intent (quest 29)", int(turn["quest"]) == 29)

	# gossip_closed hides the window.
	bus.close_gossip()
	await _wait(1)
	_check("gossip window hidden on close", not win.visible)
	win.queue_free()


func _verify_quest_log_window() -> void:
	print("[quest_log_window]")
	var bus = EventBus.new()
	var win = QuestLogWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)
	bus.publish_quest_log(_sample_log())
	win.set_frame_visible(true)
	win.render()
	await _wait(1)
	# Two quest header buttons + their objective labels are present.
	var q28 := _find_button_containing(win, "Quest #28")
	var q29 := _find_button_containing(win, "Quest #29")
	_check("quest 28 header rendered", q28 != null)
	_check("quest 29 header rendered (ready to turn in)",
		q29 != null and q29.text.find("ready to turn in") != -1)
	# Clicking a quest header watches it.
	if q29 != null:
		q29.emit_signal("pressed")
	_check("clicking quest 29 tracks it", bus.tracked_quest() == 29)
	win.queue_free()


func _verify_quest_tracker() -> void:
	print("[quest_tracker]")
	var bus = EventBus.new()
	var tracker = QuestTracker.new()
	root.add_child(tracker)
	await _wait(1)
	tracker.setup(bus)

	# Nothing tracked → tracker hidden.
	_check("tracker hidden with no quest", not tracker.visible)

	# Publish a log → auto-track → tracker shows the watched quest's objective line.
	bus.publish_quest_log(_sample_log())
	await _wait(1)
	_check("tracker visible once a quest is tracked", tracker.visible)
	var line := _find_label_containing(tracker, "3/8")
	_check("tracker shows objective 3/8 for tracked quest 28", line != null)

	# A live QUEST_PROGRESS delta updates the tracker line.
	bus.publish_quest_progress({
		"quest_id": 28, "objective_index": 0, "type": 0, "have": 5, "need": 8, "complete": false,
	})
	await _wait(1)
	_check("tracker updated to 5/8 on progress", _find_label_containing(tracker, "5/8") != null)
	tracker.queue_free()


func _verify_net_bridge() -> void:
	print("[net_bridge]")
	# The C→S frame builders produce non-empty IF-2 frames (full wire round-trip proven
	# by the C++ ctest). Constructing MeridianNetThread does NOT start the net thread.
	var net := MeridianNetThread.new()
	_check("gossip_hello frame non-empty", net.build_gossip_hello_frame(27).size() > 0)
	_check("quest_accept frame non-empty", net.build_quest_accept_frame(28, 27).size() > 0)
	_check("quest_turn_in frame non-empty", net.build_quest_turn_in_frame(29, 27, -1).size() > 0)
	_check("quest_log_request frame non-empty", net.build_quest_log_request_frame().size() > 0)

	# decode_quest_frame rejects a garbage body safely (kind "") for a quest/gossip op,
	# and returns kind "" for a non-quest opcode.
	var bad: Dictionary = net.decode_quest_frame(0x5202, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage GOSSIP_MENU → kind ''", String(bad.get("kind", "x")) == "")
	var other: Dictionary = net.decode_quest_frame(0x2001, PackedByteArray())
	_check("non-quest opcode → kind ''", String(other.get("kind", "x")) == "")


# --- helpers -----------------------------------------------------------------

func _find_button_containing(root_node: Node, needle: String) -> Button:
	for n in _walk(root_node):
		if n is Button and (n as Button).text.find(needle) != -1:
			return n
	return null


func _find_label_containing(root_node: Node, needle: String) -> Label:
	for n in _walk(root_node):
		if n is Label and (n as Label).text.find(needle) != -1:
			return n
	return null


func _walk(n: Node) -> Array:
	var out: Array = [n]
	for c in n.get_children():
		out.append_array(_walk(c))
	return out
