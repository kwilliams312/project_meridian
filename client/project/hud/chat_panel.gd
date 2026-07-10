# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the CHAT PANEL view (SOC-01, #367/#434). A panel with a scrollback
# (channel-colored, auto-following) + an input row: a channel selector (Say/Yell/Zone/
# Whisper), a whisper-target field (shown only for Whisper), and a text input. Submitting
# a line sends CHAT_MESSAGE; delivered lines (incl. SYSTEM lines — GM-command replies and
# the mute notice, sender_guid 0) render in the scrollback; a typed CHAT_REJECTED surfaces
# as a System line.
#
# PURE VIEW / MVVM (the #431 contract): this node owns NO server state and never touches
# the net thread. The HUD (the ViewModel) subscribes it to MeridianEventBus.chat_line_added
# and seeds it from bus.chat_history(); a submit turns into a bus INTENT (bus.request_chat_
# send) — the world scene owns the net thread and sends the frame. So the server stays
# authoritative: the client only asks; it never invents a line (the sender even sees its
# OWN say/yell because worldd echoes it back). '.'-prefixed lines (.help/.tele) are ordinary
# sends — the SERVER recognizes the GM command and replies with a System CHAT_DELIVER; there
# is NO client-side command parsing.
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianChatPanel
extends PanelContainer

const Bus := preload("res://hud/event_bus.gd")

const WIN_W := 420.0
const SCROLLBACK_H := 150.0

# Per-channel prefix + color for a rendered line (bbcode hex, no leading '#').
const _CHANNEL_STYLE := {
	Bus.CHAT_SAY: {"tag": "Say", "color": "e6e6e6"},
	Bus.CHAT_YELL: {"tag": "Yell", "color": "ff6a4d"},
	Bus.CHAT_WHISPER: {"tag": "Whisper", "color": "d08bff"},
	Bus.CHAT_ZONE: {"tag": "Zone", "color": "6ad0ff"},
}
const _SYSTEM_COLOR := "ffd24d"   # System lines (GM reply / mute / rejections)

var _bus: MeridianEventBus
var _scrollback: RichTextLabel
var _channel_pick: OptionButton
var _target_edit: LineEdit
var _input: LineEdit
var _built := false

# The channel ordinals in OptionButton item order (index -> ChatChannel).
var _channel_order: Array = [Bus.CHAT_SAY, Bus.CHAT_YELL, Bus.CHAT_ZONE, Bus.CHAT_WHISPER]


func _ready() -> void:
	_build()


# Bind to the world session's event bus. Idempotent per bus. Seeds the scrollback from the
# bus history so a panel bound after some traffic shows the existing lines.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.chat_line_added.connect(_on_chat_line_added)
	if not _built:
		_build()
	for entry in _bus.chat_history():
		_render_line(entry as Dictionary)


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(WIN_W, 0.0)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 4)
	add_child(root)

	# Scrollback: a following RichTextLabel (bbcode for channel colors), scrolls with new lines.
	_scrollback = RichTextLabel.new()
	_scrollback.bbcode_enabled = true
	_scrollback.scroll_following = true
	_scrollback.custom_minimum_size = Vector2(WIN_W, SCROLLBACK_H)
	_scrollback.fit_content = false
	_scrollback.selection_enabled = true
	root.add_child(_scrollback)

	# Input row: [channel v] [whisper target] [line input].
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	root.add_child(row)

	_channel_pick = OptionButton.new()
	_channel_pick.add_item("Say", Bus.CHAT_SAY)
	_channel_pick.add_item("Yell", Bus.CHAT_YELL)
	_channel_pick.add_item("Zone", Bus.CHAT_ZONE)
	_channel_pick.add_item("Whisper", Bus.CHAT_WHISPER)
	_channel_pick.select(0)
	_channel_pick.item_selected.connect(_on_channel_selected)
	row.add_child(_channel_pick)

	_target_edit = LineEdit.new()
	_target_edit.placeholder_text = "to…"
	_target_edit.custom_minimum_size = Vector2(90.0, 0.0)
	_target_edit.visible = false  # only for Whisper
	row.add_child(_target_edit)

	_input = LineEdit.new()
	_input.placeholder_text = "Press Enter to chat…  (.help for GM commands)"
	_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_input.max_length = 255
	_input.text_submitted.connect(_on_text_submitted)
	row.add_child(_input)

	_built = true


# --- Public control surface (bound by the HUD / world scene) ------------------

## Focus the input so the player can type (bound to Enter by the world scene). Building on
## demand keeps it safe before _ready().
func focus_input() -> void:
	if not _built:
		_build()
	_input.grab_focus()


## True while the text input (or whisper-target field) has keyboard focus — the world scene
## checks this to suppress gameplay keybinds (WASD / number keys) while the player is typing.
func is_input_focused() -> bool:
	if not _built:
		return false
	return _input.has_focus() or _target_edit.has_focus()


## The currently-selected ChatChannel ordinal.
func selected_channel() -> int:
	if not _built:
		return Bus.CHAT_SAY
	return int(_channel_order[_channel_pick.selected]) if _channel_pick.selected >= 0 else Bus.CHAT_SAY


## Select a channel by ChatChannel ordinal (reveals the whisper target for WHISPER). Used by
## the headless verify + any keybind that switches channel.
func set_channel(channel: int) -> void:
	if not _built:
		_build()
	var idx := _channel_order.find(channel)
	if idx >= 0:
		_channel_pick.select(idx)
		_on_channel_selected(idx)


## Set the whisper target name (used by the headless verify + a "/w name" convenience).
func set_whisper_target(name: String) -> void:
	if not _built:
		_build()
	_target_edit.text = name


## Submit `text` on the current channel/target exactly as if the player pressed Enter. Drives
## the send path headlessly (no focus needed). Clears the input on success.
func submit_line(text: String) -> void:
	if not _built:
		_build()
	_input.text = text
	_on_text_submitted(text)


## The plain-text scrollback (bbcode stripped) — for the headless verify to assert on.
func scrollback_text() -> String:
	return _scrollback.get_parsed_text() if _built else ""

# --- Bus signal handlers -----------------------------------------------------

func _on_chat_line_added(entry: Dictionary) -> void:
	_render_line(entry)

# --- Input handlers ----------------------------------------------------------

func _on_channel_selected(index: int) -> void:
	# Reveal the whisper-target field only for the Whisper channel.
	var channel := int(_channel_order[index]) if index >= 0 and index < _channel_order.size() else Bus.CHAT_SAY
	_target_edit.visible = channel == Bus.CHAT_WHISPER


func _on_text_submitted(text: String) -> void:
	if _bus == null:
		return
	var body := text.strip_edges()
	if body.is_empty():
		_input.clear()
		return
	var channel := selected_channel()
	var target := _target_edit.text.strip_edges() if channel == Bus.CHAT_WHISPER else ""
	_bus.request_chat_send(channel, target, body)
	_input.clear()
	# Release focus so gameplay keys (WASD / number keys) work again after sending.
	_input.release_focus()

# --- Rendering ---------------------------------------------------------------

# Append one scrollback line, channel-colored. A SYSTEM line (is_system) renders as
# "[System] text"; a normal line as "[Channel] Sender: text".
func _render_line(entry: Dictionary) -> void:
	if not _built:
		_build()
	var text := String(entry.get("text", ""))
	if bool(entry.get("is_system", false)):
		_scrollback.append_text("[color=#%s][System][/color] %s\n" % [_SYSTEM_COLOR, _escape(text)])
		return
	var channel := int(entry.get("channel", Bus.CHAT_SAY))
	var style: Dictionary = _CHANNEL_STYLE.get(channel, _CHANNEL_STYLE[Bus.CHAT_SAY])
	var sender := String(entry.get("sender_name", ""))
	var who := sender if not sender.is_empty() else "?"
	_scrollback.append_text("[color=#%s]\\[%s] %s:[/color] %s\n"
		% [style["color"], style["tag"], _escape(who), _escape(text)])


# Escape user text so it never injects bbcode tags into the scrollback.
static func _escape(s: String) -> String:
	return s.replace("[", "[lb]")
