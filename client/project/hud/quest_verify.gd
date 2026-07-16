# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the QUEST + GOSSIP UI (QST-01,
# #433): the event-bus quest/gossip registry, the gossip window, the quest log window,
# the quest tracker, and the MeridianNetThread quest/gossip frame builders + decode
# safety. NOT a shipped scene: run it as
#   godot --headless --script res://hud/quest_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the bus stores + re-emits the server's quest/gossip state (QUEST_LOG snapshot →
#     tracked-quest pick; QUEST_PROGRESS merge; accept / turn-in results drive tracking +
#     log removal); the PROACTIVE per-NPC overhead marker (QUEST_MARKER_UPDATE, #844/#849)
#     sets the right billboard icon per QuestMarkerKind (`!`/lit `?`/greyed `?`/hidden) on
#     SIGHT — no longer derived from GOSSIP_MENU;
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
	_verify_event_bus_quest_markers()
	await _verify_gossip_window()
	await _verify_turn_in_choice_picker()
	await _verify_quest_log_window()
	await _verify_quest_tracker()
	_verify_net_bridge()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


# A populated two-quest log snapshot (the shape decode_quest_frame produces), including
# each quest's REWARD PREVIEW (#443): quest 28 offers a 2-option choice; quest 29 is flat.
func _sample_log() -> Array:
	return [
		{
			"quest_id": 28, "level": 4, "complete": false,
			"objectives": [
				{"type": 0, "target_id": 26, "have": 3, "need": 8, "complete": false},
			],
			"reward_xp": 300, "reward_money": 1200,
			"reward_items": [{"item_id": 700, "count": 1}],
			"choice_items": [{"item_id": 801, "count": 1}, {"item_id": 802, "count": 2}],
		},
		{
			"quest_id": 29, "level": 5, "complete": true,
			"objectives": [
				{"type": 1, "target_id": 50, "have": 6, "need": 6, "complete": true},
			],
			"reward_xp": 420, "reward_money": 500,
			"reward_items": [{"item_id": 900, "count": 3}],
			"choice_items": [],
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

	# The reward PREVIEW (#443) round-trips through the bus quest entry: flat XP/copper, the
	# always-granted item, and the one-of choice options the turn-in picker reads.
	var q28: Dictionary = bus.quest_entry(28)
	_check("reward preview stored (xp/money)", int(q28.get("reward_xp", 0)) == 300 and
		int(q28.get("reward_money", 0)) == 1200)
	_check("always-granted reward item stored", (q28.get("reward_items", []) as Array).size() == 1)
	_check("choice options stored (2)", (q28.get("choice_items", []) as Array).size() == 2)
	_check("flat quest 29 has no choice options", (bus.quest_entry(29).get("choice_items", []) as Array).is_empty())

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

	# A GOSSIP_MENU stores the open menu + emits, but NO LONGER drives the overhead marker
	# (#844/#849): the marker is now push-only (QUEST_MARKER_UPDATE), so opening gossip must
	# NOT fire giver_indicator_changed nor set a giver_marker.
	var gossip_marker_fired := {"v": false}
	bus.giver_indicator_changed.connect(func(_g, _k): gossip_marker_fired["v"] = true)
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
	_check("gossip does NOT derive overhead marker (push is source of truth)",
		not bool(gossip_marker_fired["v"]))
	_check("gossip leaves giver_marker at NONE", bus.giver_marker(27) == EventBus.MARKER_NONE)

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


# The push-driven overhead marker (#844/#849): a QUEST_MARKER_UPDATE for an NPC sets the
# right icon per QuestMarkerKind, transitions update it, and NONE hides it — mirroring the
# world scene's state->glyph/colour mapping. Proves `!` shows on SIGHT (no gossip needed).
func _verify_event_bus_quest_markers() -> void:
	print("[event_bus/quest_markers]")
	var bus = EventBus.new()

	# world.gd's mapping under test (kind -> {glyph, is_dimmed}). Kept beside the assertions
	# so the state->icon contract the billboard renders is verified headlessly.
	var icon_of := func(kind: int) -> Dictionary:
		match kind:
			EventBus.MARKER_AVAILABLE: return {"glyph": "!", "dimmed": false}
			EventBus.MARKER_TURN_IN_READY: return {"glyph": "?", "dimmed": false}
			EventBus.MARKER_TURN_IN_INCOMPLETE: return {"glyph": "?", "dimmed": true}
			_: return {"glyph": "", "dimmed": false}  # NONE -> hidden

	var events: Array = []
	bus.giver_indicator_changed.connect(func(g, k): events.append([g, k]))

	# On SIGHT: worldd pushes AVAILABLE for Tansy (guid 27) before any interaction -> gold `!`.
	bus.publish_quest_marker_update(27, EventBus.MARKER_AVAILABLE)
	_check("on-sight AVAILABLE emits for npc 27",
		events.size() == 1 and int(events[0][0]) == 27 and int(events[0][1]) == EventBus.MARKER_AVAILABLE)
	_check("giver_marker(27) == AVAILABLE", bus.giver_marker(27) == EventBus.MARKER_AVAILABLE)
	_check("AVAILABLE maps to gold '!'", icon_of.call(bus.giver_marker(27)) == {"glyph": "!", "dimmed": false})

	# A re-push of the SAME kind is a no-op (diffed) — no duplicate emit.
	bus.publish_quest_marker_update(27, EventBus.MARKER_AVAILABLE)
	_check("re-push of unchanged marker is a no-op", events.size() == 1)

	# ACCEPT clears Tansy's `!` (server pushes NONE) -> marker hidden.
	bus.publish_quest_marker_update(27, EventBus.MARKER_NONE)
	_check("accept clears '!' (NONE emitted)",
		events.size() == 2 and int(events[1][1]) == EventBus.MARKER_NONE)
	_check("giver_marker(27) back to NONE", bus.giver_marker(27) == EventBus.MARKER_NONE)
	_check("NONE maps to no glyph (hidden)", String(icon_of.call(EventBus.MARKER_NONE)["glyph"]) == "")

	# Bram (guid 55): greyed `?` while the objective is incomplete …
	bus.publish_quest_marker_update(55, EventBus.MARKER_TURN_IN_INCOMPLETE)
	_check("Bram TURN_IN_INCOMPLETE stored", bus.giver_marker(55) == EventBus.MARKER_TURN_IN_INCOMPLETE)
	_check("INCOMPLETE maps to dimmed '?'",
		icon_of.call(bus.giver_marker(55)) == {"glyph": "?", "dimmed": true})

	# … then lit `?` once the objective completes (a distinct transition, not a no-op).
	bus.publish_quest_marker_update(55, EventBus.MARKER_TURN_IN_READY)
	_check("objective done -> TURN_IN_READY transition emits",
		events.size() == 4 and int(events[3][1]) == EventBus.MARKER_TURN_IN_READY)
	_check("READY maps to lit '?'", icon_of.call(bus.giver_marker(55)) == {"glyph": "?", "dimmed": false})

	# … and NONE after turn-in clears Bram's `?`.
	bus.publish_quest_marker_update(55, EventBus.MARKER_NONE)
	_check("turn-in clears Bram's '?'", bus.giver_marker(55) == EventBus.MARKER_NONE)


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

	# #843: the in-progress reminder reads its objective tally from the bus quest log,
	# so seed quest #40 as accepted-but-incomplete (1 of 3 kobolds culled) before the menu.
	bus.publish_quest_log([
		{
			"quest_id": 40, "level": 2, "complete": false,
			"objectives": [
				{"type": 0, "target_id": 26, "have": 1, "need": 3, "complete": false},
			],
			"reward_xp": 50, "reward_money": 120,
			"reward_items": [], "choice_items": [],
		},
	])

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

	# #843: the IN_PROGRESS row is a non-interactive reminder — NOT a re-offer. It must
	# render as a Label carrying "Did you complete this?" + the live objective tally
	# (1/3), and there must be NO accept button for the accepted quest #40.
	var reminder := _find_label_containing(win, "Quest #40")
	_check("in-progress row renders as a reminder label (not an accept button)",
		reminder != null and _find_button_containing(win, "Quest #40") == null)
	_check("in-progress reminder asks 'Did you complete this?'",
		reminder != null and reminder.text.find("Did you complete this?") != -1)
	_check("in-progress reminder shows the objective tally (1/3)",
		reminder != null and reminder.text.find("(1/3)") != -1)
	_check("no accept offer is shown for the already-accepted quest #40",
		_find_button_containing(win, "Accept quest #40") == null)

	# #838: a SUCCESSFUL accept (OK) re-requests the gossip menu for the open NPC so the
	# dialog transitions (available → in-progress/turn-in) instead of leaving the stale
	# "Accept" row. A GATED accept (status != OK) must NOT re-request.
	var refresh := {"npc": 0, "count": 0}
	bus.gossip_hello_requested.connect(func(g): refresh["npc"] = g; refresh["count"] += 1)
	bus.publish_quest_accept_result(28, 4)  # LEVEL_TOO_LOW → no refresh
	_check("gated accept does NOT refresh the gossip menu", int(refresh["count"]) == 0)
	bus.publish_quest_accept_result(28, 0)  # OK → re-request gossip on the open NPC 27
	_check("accept OK re-requests gossip menu on the open NPC (27)",
		int(refresh["count"]) == 1 and int(refresh["npc"]) == 27)

	# #850: a SUCCESSFUL turn-in (OK) likewise re-requests the gossip menu for the open NPC
	# so the completed quest's turn-in row disappears (server drops it once QuestLog marks
	# the quest done) — the NPC returns to normal instead of leaving a stale, spammable
	# "Turn in" row that only Esc clears. A failed turn-in (status != OK) must NOT re-request.
	bus.publish_quest_turn_in_result({"quest_id": 29, "status": 4})  # INCOMPLETE → no refresh
	_check("failed turn-in does NOT refresh the gossip menu", int(refresh["count"]) == 1)
	bus.publish_quest_turn_in_result({"quest_id": 29, "status": 0})  # OK → re-request on NPC 27
	_check("turn-in OK re-requests gossip menu on the open NPC (27)",
		int(refresh["count"]) == 2 and int(refresh["npc"]) == 27)

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


# The turn-in choice picker (#443/#442): a CHOICE-reward quest opens a picker with the
# reward preview + one button per option; the picked index rides QUEST_TURN_IN.choice_index
# (fixing the old choice_index=-1 → BAD_CHOICE). A flat-reward quest still turns in with -1.
func _verify_turn_in_choice_picker() -> void:
	print("[gossip_window/choice_picker]")
	var bus = EventBus.new()
	var win = GossipWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	# The quest reward preview lives in the bus quest log; publish it, then open gossip on a
	# turn-in NPC offering BOTH a choice-reward quest (28) and a flat-reward quest (29).
	bus.publish_quest_log(_sample_log())
	bus.publish_gossip_menu(88, [
		{"kind": EventBus.GOSSIP_QUEST_COMPLETE, "target_id": 28},  # choice quest
		{"kind": EventBus.GOSSIP_QUEST_COMPLETE, "target_id": 29},  # flat quest
	])
	await _wait(1)

	var turn: Array = []
	bus.quest_turn_in_requested.connect(func(q, n, c): turn.append([q, n, c]))

	# A FLAT-reward quest (29) turns in immediately with choice_index -1 (unchanged path).
	var flat_btn := _find_button_containing(win, "Turn in quest #29")
	_check("flat turn-in row rendered", flat_btn != null)
	if flat_btn != null:
		flat_btn.emit_signal("pressed")
	_check("flat quest turns in with choice_index -1",
		turn.size() == 1 and int(turn[0][0]) == 29 and int(turn[0][2]) == -1)

	# A CHOICE-reward quest (28) opens the picker instead of turning in immediately.
	var choice_btn := _find_button_containing(win, "Turn in quest #28")
	_check("choice turn-in row rendered", choice_btn != null)
	if choice_btn != null:
		choice_btn.emit_signal("pressed")
	await _wait(1)
	_check("choice quest opens the picker (no immediate turn-in)", turn.size() == 1)
	_check("reward preview shows XP", _find_label_containing(win, "300 XP") != null)
	var opt0 := _find_button_containing(win, "Item #801")
	var opt1 := _find_button_containing(win, "Item #802")
	_check("both choice options rendered", opt0 != null and opt1 != null)

	# Picking the SECOND option sends choice_index 1 (quest 28, npc 88) — the #442 fix.
	if opt1 != null:
		opt1.emit_signal("pressed")
	_check("picking option 1 sends choice_index 1",
		turn.size() == 2 and int(turn[1][0]) == 28 and int(turn[1][1]) == 88 and int(turn[1][2]) == 1)
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
	# QUEST_MARKER_UPDATE (0x4007, #844/#849) rejects a garbage body safely (kind ''); the
	# valid wire round-trip for this opcode is proven by the C++ ctest (clientnet quest case).
	var bad_marker: Dictionary = net.decode_quest_frame(0x4007, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage QUEST_MARKER_UPDATE → kind ''", String(bad_marker.get("kind", "x")) == "")


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
