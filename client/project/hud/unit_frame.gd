# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — a single HUD UNIT FRAME view (UI-01, #431): name + level, a
# health bar, and a power bar colored by power_type. Used for BOTH the local player
# frame and the target frame — same widget, different binding (see hud/hud.gd).
#
# PURE VIEW (MVVM): this node owns NO server state and NO subscriptions. It renders
# whatever vitals Dictionary render() is handed (the canonical MeridianEventBus
# record shape) and nothing else. The HUD (the ViewModel) subscribes to the event
# bus and calls render()/set_frame_visible() — so this view is a leaf that can be
# reused, restyled, or replaced by a UI-02 addon without touching data flow.
#
# EVENT-DRIVEN, ZERO per-frame cost (≤2 ms budget, #431): no _process here. render()
# runs only when the HUD relays a bus event. Widgets are built once in _ready();
# updates are label-text + bar-width writes, no node allocation on the hot path.
#
# Built in code (like scenes/world/world.gd / camera_demo.gd) so there is no .tscn to
# keep in sync — the frame is self-contained.
class_name MeridianUnitFrame
extends PanelContainer

const PowerColors := preload("res://hud/power_colors.gd")

const BAR_W := 220.0
const BAR_H := 16.0

var _name_label: Label
var _health_fill: ColorRect
var _health_text: Label
var _power_bar: Control
var _power_fill: ColorRect
var _power_text: Label

var _built := false
var _pending: Dictionary = {}
var _has_pending := false


func _ready() -> void:
	_build()
	if _has_pending:
		_apply(_pending)
		_has_pending = false


# Render a canonical vitals record (MeridianEventBus shape). Safe to call before the
# node is in the tree — the latest record is stashed and applied on _ready().
func render(vitals: Dictionary) -> void:
	if not _built:
		_pending = vitals.duplicate()
		_has_pending = true
		return
	_apply(vitals)


# Show or hide the whole frame (the target frame hides when there is no target).
func set_frame_visible(v: bool) -> void:
	visible = v


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(BAR_W + 20.0, 0.0)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 3)
	add_child(root)

	# Name + level line.
	_name_label = Label.new()
	_name_label.add_theme_color_override("font_color", Color.WHITE)
	_name_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_name_label.add_theme_constant_override("outline_size", 4)
	root.add_child(_name_label)

	# Health bar (red->green by ratio) with an overlaid "cur / max" readout.
	var hb := _make_bar(Color(0.20, 0.75, 0.30))
	_health_fill = hb[0]
	_health_text = hb[1]
	root.add_child(hb[2])

	# Power bar (colored by power_type). Hidden for a unit with no secondary pool.
	var pb := _make_bar(PowerColors.color_for(PowerColors.MANA))
	_power_fill = pb[0]
	_power_text = pb[1]
	_power_bar = pb[2]
	root.add_child(_power_bar)

	_built = true


# Build one bar: [fill:ColorRect, text:Label, bar:Control]. The bar is a fixed-size
# Control holding a dark background, a colored fill (left-aligned, width set by
# ratio), and a centered readout label.
func _make_bar(fill_color: Color) -> Array:
	var bar := Control.new()
	bar.custom_minimum_size = Vector2(BAR_W, BAR_H)
	bar.size_flags_horizontal = 0  # shrink-begin: keep the fixed BAR_W, don't stretch

	var bg := ColorRect.new()
	bg.color = Color(0.08, 0.08, 0.10, 0.85)
	bg.position = Vector2.ZERO
	bg.size = Vector2(BAR_W, BAR_H)
	bar.add_child(bg)

	var fill := ColorRect.new()
	fill.color = fill_color
	fill.position = Vector2.ZERO
	fill.size = Vector2(BAR_W, BAR_H)
	bar.add_child(fill)

	var text := Label.new()
	text.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	text.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	text.position = Vector2.ZERO
	text.size = Vector2(BAR_W, BAR_H)
	text.add_theme_color_override("font_color", Color.WHITE)
	text.add_theme_color_override("font_outline_color", Color.BLACK)
	text.add_theme_constant_override("outline_size", 3)
	text.add_theme_font_size_override("font_size", 11)
	bar.add_child(text)

	return [fill, text, bar]


func _apply(v: Dictionary) -> void:
	var display_name := String(v.get("name", ""))
	if display_name.is_empty():
		display_name = "Unknown"
	var level := int(v.get("level", 0))
	_name_label.text = "%s   Lv %d" % [display_name, level] if level > 0 else display_name

	var health := int(v.get("health", 0))
	var max_health := int(v.get("max_health", 0))
	var h_ratio := (float(health) / float(max_health)) if max_health > 0 else 0.0
	h_ratio = clampf(h_ratio, 0.0, 1.0)
	_health_fill.size = Vector2(BAR_W * h_ratio, BAR_H)
	# Red at 0%, green at 100% — a cheap at-a-glance danger cue.
	_health_fill.color = Color(0.85, 0.20, 0.20).lerp(Color(0.20, 0.75, 0.30), h_ratio)
	_health_text.text = ("%d / %d" % [health, max_health]) if max_health > 0 else "—"

	var power := int(v.get("power", 0))
	var max_power := int(v.get("max_power", 0))
	var power_type := int(v.get("power_type", PowerColors.NONE))
	if PowerColors.has_power(power_type, max_power):
		_power_bar.visible = true
		var p_ratio := clampf(float(power) / float(max_power), 0.0, 1.0)
		_power_fill.size = Vector2(BAR_W * p_ratio, BAR_H)
		_power_fill.color = PowerColors.color_for(power_type)
		_power_text.text = "%d / %d" % [power, max_power]
	else:
		_power_bar.visible = false  # NONE / no pool — no power bar (basic melee unit)
