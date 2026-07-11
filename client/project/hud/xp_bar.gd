# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the XP BAR view (CHR-03, #531): a slim progress bar along the
# bottom of the HUD that fills toward the next level, plus a brief level-up burst.
#
# PURE VIEW / MVVM (the #431 contract): owns NO server state and NO net handle. It binds
# to MeridianEventBus.xp_changed (an XP_GAINED award — fill toward the next level) and
# MeridianEventBus.leveled_up (a LEVEL_UP ding — flash + show the new level). The client
# NEVER runs the XP curve (Principle 1 "server is law") — the bar renders exactly the
# `current / next_level_at` progress the server computed.
#
# PERFORMANCE (≤2 ms / no per-frame alloc, #431): idle, ZERO work — no _process. Updates
# are a ColorRect width write + a label text write on the bus signal (a handful of times
# per session). The level-up burst is a one-shot Tween (Godot drives it), disabled the
# instant it finishes — no polling.
#
# Built in code (like unit_frame.gd / cast_bar.gd) — self-contained, no .tscn.
class_name MeridianXpBar
extends Control

const BAR_W := 292.0
const BAR_H := 12.0

# The "rested-purple" XP fill, matching the HUD's dark panels + warm accents.
const XP_COLOR := Color(0.55, 0.35, 0.85)
const XP_FLASH_COLOR := Color(1.0, 0.95, 0.6)  # brief ding highlight

var _bus: MeridianEventBus
var _bg: ColorRect
var _fill: ColorRect
var _label: Label
var _burst: Label
var _built := false


func _ready() -> void:
	_build()


# Bind to the world session's event bus (called once by the HUD). Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.xp_changed.connect(_on_xp_changed)
	_bus.leveled_up.connect(_on_leveled_up)
	# Paint the initial state (the bus may already hold XP from a queued XP_GAINED).
	_on_xp_changed(_bus.xp_current(), _bus.xp_to_next(), _bus.xp_level())


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(BAR_W, BAR_H)

	_bg = ColorRect.new()
	_bg.color = Color(0.08, 0.08, 0.10, 0.9)
	_bg.position = Vector2.ZERO
	_bg.size = Vector2(BAR_W, BAR_H)
	add_child(_bg)

	_fill = ColorRect.new()
	_fill.color = XP_COLOR
	_fill.position = Vector2.ZERO
	_fill.size = Vector2(0.0, BAR_H)
	add_child(_fill)

	_label = Label.new()
	_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_label.position = Vector2.ZERO
	_label.size = Vector2(BAR_W, BAR_H)
	_label.add_theme_color_override("font_color", Color.WHITE)
	_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_label.add_theme_constant_override("outline_size", 3)
	_label.add_theme_font_size_override("font_size", 10)
	add_child(_label)

	# Level-up burst: a bright transient label centered ABOVE the bar. Hidden until a ding.
	_burst = Label.new()
	_burst.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_burst.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_burst.position = Vector2(0.0, -34.0)
	_burst.size = Vector2(BAR_W, 26.0)
	_burst.add_theme_color_override("font_color", Color(1.0, 0.95, 0.6))
	_burst.add_theme_color_override("font_outline_color", Color.BLACK)
	_burst.add_theme_constant_override("outline_size", 5)
	_burst.add_theme_font_size_override("font_size", 22)
	_burst.modulate = Color(1, 1, 1, 0)  # transparent until a ding
	add_child(_burst)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

# XP progress changed (XP_GAINED). Fill toward the next level + show "Lv N  cur / next".
func _on_xp_changed(current: int, next_level_at: int, level: int) -> void:
	if not _built:
		_build()
	var ratio := clampf(float(current) / float(next_level_at), 0.0, 1.0) if next_level_at > 0 else 0.0
	_fill.size = Vector2(BAR_W * ratio, BAR_H)
	if next_level_at > 0:
		_label.text = "Lv %d   %d / %d  (%d%%)" % [level, current, next_level_at, int(round(ratio * 100.0))]
	else:
		# No threshold yet (pre-first-award) / max level — show the level, no ratio.
		_label.text = "Lv %d" % level if level > 0 else ""


# The player leveled up (LEVEL_UP). Flash the fill + show a brief "Level Up!  Lv N" burst.
func _on_leveled_up(new_level: int, _stat_growth: Dictionary) -> void:
	if not _built:
		_build()
	_label.text = "Lv %d" % new_level
	_burst.text = "Level Up!   Lv %d" % new_level
	_play_burst()


# One-shot ding presentation: flash the fill bright then back, and fade the burst label in
# and out. Guarded so it is a no-op headlessly if the SceneTree tween is unavailable.
func _play_burst() -> void:
	# Fill flash: snap to the highlight, ease back to the XP color.
	_fill.color = XP_FLASH_COLOR
	var flash := create_tween()
	if flash != null:
		flash.tween_property(_fill, "color", XP_COLOR, 0.6)
	# Burst label: pop to full opacity, hold briefly, fade out.
	_burst.modulate = Color(1, 1, 1, 1)
	var fade := create_tween()
	if fade != null:
		fade.tween_interval(0.7)
		fade.tween_property(_burst, "modulate:a", 0.0, 0.8)
	else:
		# No tween (headless with no processing) — leave the burst shown; the model
		# (level text + burst text) is what the headless verify asserts.
		pass
