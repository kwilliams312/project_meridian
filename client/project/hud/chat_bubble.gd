# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — a floating CHAT BUBBLE (SOC-01, #367/#434): a billboarded Label3D that
# the world scene attaches over an entity node to show that entity's most-recent SAY/YELL
# line for a few seconds. YELL bubbles read hotter (color) than SAY. Tied to the entity node
# (a child of the remote/local player node) so it tracks the entity as it moves — the world
# scene owns the entity nodes; the bus only tells it a bubble is due (chat_bubble_requested).
#
# EVENT-DRIVEN (≤2 ms budget, #431): no _process. A bubble appears on show_message() and
# hides itself via a one-shot SceneTreeTimer — the only cost is a couple of timers per line,
# never per-frame work. A new line on the same entity just refreshes the text + restarts the
# timer (the bubble is reused, not re-allocated).
class_name MeridianChatBubble
extends Label3D

const Bus := preload("res://hud/event_bus.gd")

# How long a bubble stays up (seconds) before it hides. Yell lingers a touch longer.
const SAY_LIFETIME := 5.0
const YELL_LIFETIME := 7.0

# Bubble color per spatial channel (say = soft white, yell = hot orange).
const _SAY_COLOR := Color(0.94, 0.94, 0.90)
const _YELL_COLOR := Color(1.0, 0.55, 0.30)

var _generation := 0   # bumped on each show; a stale timer callback is ignored


func _init() -> void:
	billboard = BaseMaterial3D.BILLBOARD_ENABLED
	no_depth_test = true
	font_size = 40
	outline_size = 10
	modulate = _SAY_COLOR
	# Sit above the nameplate/marker stack on a ~1.8 m capsule.
	position = Vector3(0.0, 3.0, 0.0)
	width = 400
	autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	visible = false


# Show `text` for this entity's line on `channel` (SAY/YELL). Refreshes an existing bubble in
# place and restarts its hide timer. A whisper/zone/system line never calls this (only spatial
# channels bubble — the bus enforces that in publish_chat_deliver).
func show_message(text: String, channel: int) -> void:
	self.text = text
	modulate = _YELL_COLOR if channel == Bus.CHAT_YELL else _SAY_COLOR
	visible = true
	_generation += 1
	var my_gen := _generation
	var lifetime := YELL_LIFETIME if channel == Bus.CHAT_YELL else SAY_LIFETIME
	# One-shot hide — reused, so a newer message (higher generation) cancels this one.
	if is_inside_tree():
		var timer := get_tree().create_timer(lifetime)
		timer.timeout.connect(func(): _expire(my_gen))


# Hide the bubble iff no newer message replaced it since this timer was armed.
func _expire(generation: int) -> void:
	if generation == _generation:
		visible = false
		text = ""
