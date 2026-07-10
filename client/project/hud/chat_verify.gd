# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the CHAT panel + bubbles + channels
# (SOC-01, #367/#434). NOT a shipped scene: run it as
#   godot --headless --path client/project --script res://hud/chat_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the event bus stores + re-emits chat state (CHAT_DELIVER scrollback + history seeding,
#     the SAY/YELL bubble signal, SYSTEM lines with sender_guid 0 that do NOT bubble, and a
#     typed CHAT_REJECTED surfaced as a System line);
#   * the chat panel renders delivered/System lines into its scrollback, switches channels
#     (whisper reveals a target field), and turns a submit into a bus send INTENT — including
#     a '.'-prefixed GM-command line, which goes out as an ordinary CHAT_MESSAGE;
#   * a chat bubble shows the entity's line;
#   * MeridianNetThread.decode_chat_frame() safely handles garbage (kind "") and the real
#     opcodes. The full wire round-trip is proven by the C++ ctest (clientnet's chat cases +
#     the cross-decode of the server's frozen chat golden bins).
# Exits 0 on success, 1 on any failed assertion — same shape as hud_verify.gd.
extends SceneTree

const EventBus := preload("res://hud/event_bus.gd")
const ChatPanel := preload("res://hud/chat_panel.gd")
const ChatBubble := preload("res://hud/chat_bubble.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian CHAT panel + bubbles + channels RUNTIME verify (#434)")

	_verify_event_bus_chat()
	await _verify_chat_panel()
	_verify_chat_bubble()
	_verify_decode_safety()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


func _verify_event_bus_chat() -> void:
	print("[event_bus/chat]")
	var bus = EventBus.new()

	# A player SAY line: stored in history, emits chat_line_added AND a bubble (spatial).
	var lines: Array = []
	var bubbles: Array = []
	bus.chat_line_added.connect(func(e): lines.append(e))
	bus.chat_bubble_requested.connect(func(guid, sender, text, ch): bubbles.append(
		{"guid": guid, "sender": sender, "text": text, "channel": ch}))
	bus.publish_chat_deliver(EventBus.CHAT_SAY, 0x2A, "Aldric", "hail, travellers")
	_check("say line added", lines.size() == 1 and String((lines[0] as Dictionary).get("text", "")) == "hail, travellers")
	_check("say line not system", not bool((lines[0] as Dictionary).get("is_system", true)))
	_check("say floats a bubble", bubbles.size() == 1 and int((bubbles[0] as Dictionary).get("guid", 0)) == 0x2A)
	_check("bubble carries channel SAY", int((bubbles[0] as Dictionary).get("channel", -1)) == EventBus.CHAT_SAY)

	# A YELL line also bubbles (wider spatial channel).
	bus.publish_chat_deliver(EventBus.CHAT_YELL, 0x2A, "Aldric", "TO ARMS!")
	_check("yell floats a bubble", bubbles.size() == 2 and int((bubbles[1] as Dictionary).get("channel", -1)) == EventBus.CHAT_YELL)

	# A ZONE line is added but does NOT bubble (not spatial).
	bus.publish_chat_deliver(EventBus.CHAT_ZONE, 0x30, "Cass", "LFG dungeon")
	_check("zone line added", lines.size() == 3)
	_check("zone does NOT bubble", bubbles.size() == 2)

	# A SYSTEM line (sender_guid 0 — a GM .help reply): added as a system line, no bubble.
	bus.publish_chat_deliver(EventBus.CHAT_WHISPER, 0, "System", "Commands: .help")
	var sysline: Dictionary = lines[-1]
	_check("system line flagged is_system", bool(sysline.get("is_system", false)))
	_check("system line does NOT bubble", bubbles.size() == 2)

	# A typed CHAT_REJECTED surfaces as a System line with a human message (target echoed).
	bus.publish_chat_rejected(EventBus.CHAT_WHISPER, EventBus.CHAT_REJECT_TARGET_OFFLINE, "Brynn")
	var rej: Dictionary = lines[-1]
	_check("reject is a system line", bool(rej.get("is_system", false)))
	_check("reject names the offline target", String(rej.get("text", "")).find("Brynn") != -1)

	# The muted notice + rate-limit reason produce distinct human text.
	bus.publish_chat_rejected(EventBus.CHAT_SAY, EventBus.CHAT_REJECT_RATE_LIMITED, "")
	_check("rate-limit reason text", String((lines[-1] as Dictionary).get("text", "")).to_lower().find("quickly") != -1)

	# History accumulates the delivered + rejected lines (bounded), readable by a late binder.
	_check("history holds all lines", bus.chat_history().size() == lines.size())
	_check("channel name helper", EventBus.chat_channel_name(EventBus.CHAT_WHISPER) == "whisper")


func _verify_chat_panel() -> void:
	print("[chat_panel]")
	var bus = EventBus.new()
	# Seed one line BEFORE binding — the panel must pick it up from history on setup().
	bus.publish_chat_deliver(EventBus.CHAT_SAY, 0x2A, "Aldric", "seeded hello")

	var panel = ChatPanel.new()
	root.add_child(panel)
	await _wait(1)  # let _ready() build the widgets
	panel.setup(bus)
	await _wait(1)
	_check("panel seeded from history", panel.scrollback_text().find("seeded hello") != -1)

	# A live delivered line renders into the scrollback (bus -> panel).
	bus.publish_chat_deliver(EventBus.CHAT_ZONE, 0x30, "Cass", "live zone line")
	await _wait(1)
	_check("panel renders a live line", panel.scrollback_text().find("live zone line") != -1)
	_check("panel shows the channel tag", panel.scrollback_text().find("Zone") != -1)

	# Channel switching: WHISPER reveals the target field; SAY hides it.
	panel.set_channel(EventBus.CHAT_WHISPER)
	await _wait(1)
	_check("whisper reveals target field", panel._target_edit.visible)
	_check("selected channel is whisper", panel.selected_channel() == EventBus.CHAT_WHISPER)
	panel.set_channel(EventBus.CHAT_SAY)
	await _wait(1)
	_check("say hides target field", not panel._target_edit.visible)

	# Submit a SAY line → the panel emits a CHAT_MESSAGE send intent; input clears.
	var sends: Array = []
	bus.chat_send_requested.connect(func(ch, tgt, txt): sends.append(
		{"channel": ch, "target": tgt, "text": txt}))
	panel.submit_line("well met")
	_check("submit emitted a send intent", sends.size() == 1)
	_check("send carries SAY + text", int((sends[0] as Dictionary).get("channel", -1)) == EventBus.CHAT_SAY
		and String((sends[0] as Dictionary).get("text", "")) == "well met")
	_check("input cleared after submit", panel._input.text == "")

	# A whisper submit carries the target name.
	panel.set_channel(EventBus.CHAT_WHISPER)
	panel.set_whisper_target("Brynn")
	panel.submit_line("meet me at the gate")
	_check("whisper send carries target", String((sends[-1] as Dictionary).get("target", "")) == "Brynn")

	# A '.'-prefixed GM command goes out as an ORDINARY CHAT_MESSAGE (no client-side parsing).
	panel.set_channel(EventBus.CHAT_SAY)
	panel.submit_line(".help")
	_check("GM command sent as chat", String((sends[-1] as Dictionary).get("text", "")) == ".help")

	# Empty / whitespace submit is dropped (no send, no crash).
	var before := sends.size()
	panel.submit_line("   ")
	_check("blank submit dropped", sends.size() == before)

	panel.queue_free()


func _verify_chat_bubble() -> void:
	print("[chat_bubble]")
	var bubble = ChatBubble.new()
	root.add_child(bubble)
	bubble.show_message("hail, travellers", EventBus.CHAT_SAY)
	_check("bubble shows the line text", bubble.text == "hail, travellers")
	_check("bubble is visible", bubble.visible)
	var say_color: Color = bubble.modulate
	# A YELL line reuses the bubble in place with a hotter color.
	bubble.show_message("TO ARMS!", EventBus.CHAT_YELL)
	_check("bubble refreshed to yell text", bubble.text == "TO ARMS!")
	_check("yell color differs from say", bubble.modulate != say_color)
	bubble.queue_free()


func _verify_decode_safety() -> void:
	print("[decode_safety]")
	# decode_chat_frame must reject a garbage body safely (kind "") for both chat opcodes —
	# never crash. The real round-trip is proven in the C++ ctest.
	var net := MeridianNetThread.new()
	var d: Dictionary = net.decode_chat_frame(0x6002, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage CHAT_DELIVER → kind ''", String(d.get("kind", "x")) == "")
	var r: Dictionary = net.decode_chat_frame(0x6003, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage CHAT_REJECTED → kind ''", String(r.get("kind", "x")) == "")
	# A non-chat opcode also yields kind "" so the world scene's decode cascade falls through.
	var n: Dictionary = net.decode_chat_frame(0x2001, PackedByteArray())
	_check("non-chat opcode → kind ''", String(n.get("kind", "x")) == "")
