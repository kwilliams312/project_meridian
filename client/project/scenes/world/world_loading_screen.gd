# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the ENTER-WORLD / map-change loading screen (WLD-01, #558,
# Epic #22 Story E). A full-screen opaque overlay shown the moment the world scene
# starts mounting a zone pack, held UNTIL the spawn chunks are resident, then
# revealed. Per client-prd §M1 the loading screen appears ONLY on a map change
# (enter-world or a zone transition) — never between the seamless streamed chunks
# of an already-loaded map, which flow in behind the running view.
#
# PURE VIEW: it owns no server state and no net handle. The world scene drives it —
# begin(zone) to raise it, set_progress(text) for the streamed-in count, finish()
# to reveal the world. Built in code (like the HUD views), self-contained, no .tscn.
#
# It sits on a HIGH CanvasLayer so it draws over the 3D world AND the HUD while up.
class_name MeridianWorldLoadingScreen
extends CanvasLayer

var _dim: ColorRect          # opaque backdrop that hides the still-streaming world
var _title: Label            # "Entering <zone>…"
var _status: Label           # streamed-in progress line
var _active := false


func _ready() -> void:
	layer = 128               # above the HUD (its own CanvasLayer) and the 3D view
	_build()
	hide_now()


func _build() -> void:
	_dim = ColorRect.new()
	_dim.color = Color(0.03, 0.05, 0.08, 1.0)   # opaque — the world behind is hidden
	_dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	_dim.mouse_filter = Control.MOUSE_FILTER_STOP   # swallow clicks while loading
	add_child(_dim)

	var box := VBoxContainer.new()
	box.set_anchors_preset(Control.PRESET_CENTER)
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	# Center the stack (anchors put its top-left at screen centre; offset back by
	# a fixed half-size so the text sits centred without needing a layout pass).
	box.position = Vector2(-200, -40)
	box.custom_minimum_size = Vector2(400, 80)
	_dim.add_child(box)

	_title = _make_label(32)
	_title.text = "Entering the world…"
	box.add_child(_title)

	_status = _make_label(18)
	_status.text = "Streaming terrain…"
	_status.modulate = Color(0.75, 0.82, 0.9)
	box.add_child(_status)


func _make_label(font_size: int) -> Label:
	var l := Label.new()
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	l.add_theme_font_size_override("font_size", font_size)
	l.add_theme_color_override("font_color", Color.WHITE)
	l.add_theme_color_override("font_outline_color", Color.BLACK)
	l.add_theme_constant_override("outline_size", 4)
	return l


# Raise the loading screen for entering `zone` (a display id). Idempotent.
func begin(zone: String = "") -> void:
	_active = true
	visible = true
	if _title != null:
		_title.text = "Entering %s…" % zone if not zone.is_empty() else "Entering the world…"
	if _status != null:
		_status.text = "Streaming terrain…"


# Update the progress line (e.g. "3 / 9 chunks resident"). No-op once revealed.
func set_progress(text: String) -> void:
	if _active and _status != null:
		_status.text = text


# Reveal the world: hide the overlay. Called once the spawn chunks are resident.
func finish() -> void:
	_active = false
	visible = false


func hide_now() -> void:
	_active = false
	visible = false


func is_active() -> bool:
	return _active
