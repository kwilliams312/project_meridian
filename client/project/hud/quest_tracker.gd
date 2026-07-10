# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the on-HUD QUEST TRACKER view (QST-01, #433). A compact, always-on
# panel (top-right) showing the WATCHED quest's title + its objectives, updated LIVE
# from QUEST_PROGRESS events on the bus. This is the at-a-glance "what am I doing"
# readout that a player follows without opening the full quest log.
#
# PURE VIEW / MVVM (#431 contract): owns no server state, never touches the net thread,
# and does NO per-frame work (≤2 ms budget) — it re-renders ONLY when the bus emits a
# tracked-quest change, a QUEST_PROGRESS delta, or a QUEST_LOG resync. The HUD
# subscribes it to MeridianEventBus and it reads the tracked quest through the getter.
#
# Built in code (like unit_frame.gd) — self-contained, no .tscn.
class_name MeridianQuestTracker
extends PanelContainer

const _OBJ_TYPE_LABEL := {0: "Slay", 1: "Collect", 2: "Deliver", 3: "Explore"}

var _bus: MeridianEventBus
var _title: Label
var _body: VBoxContainer
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.tracked_quest_changed.connect(_on_tracked_changed)
	_bus.quest_progress_changed.connect(_on_progress_changed)
	_bus.quest_log_changed.connect(_on_log_changed)
	render()


func set_frame_visible(v: bool) -> void:
	visible = v


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(230.0, 0.0)
	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 2)
	add_child(root)

	_title = Label.new()
	_title.add_theme_color_override("font_color", Color(1.0, 0.88, 0.5))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 13)
	root.add_child(_title)

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 1)
	root.add_child(_body)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_tracked_changed(_quest_id: int) -> void:
	render()


func _on_progress_changed(quest_id: int, _i: int, _h: int, _n: int, _c: bool) -> void:
	# Only re-render when the delta is for the quest we are watching.
	if _bus != null and quest_id == _bus.tracked_quest():
		render()


func _on_log_changed(_quests: Array) -> void:
	render()


# --- Rendering ---------------------------------------------------------------

func render() -> void:
	if not _built or _bus == null:
		return
	var entry := _bus.tracked_quest_entry()
	for c in _body.get_children():
		c.queue_free()
	if entry.is_empty():
		set_frame_visible(false)  # nothing tracked — hide the tracker entirely
		return
	set_frame_visible(true)
	var complete := bool(entry.get("complete", false))
	_title.text = "◈ Quest #%d%s" % [int(entry.get("quest_id", 0)),
		"  (ready!)" if complete else ""]
	_title.add_theme_color_override("font_color",
		Color(0.55, 0.95, 0.45) if complete else Color(1.0, 0.88, 0.5))
	for o in entry.get("objectives", []):
		_body.add_child(_objective_line(o as Dictionary))


func _objective_line(o: Dictionary) -> Label:
	var l := Label.new()
	l.add_theme_color_override("font_outline_color", Color.BLACK)
	l.add_theme_constant_override("outline_size", 3)
	l.add_theme_font_size_override("font_size", 12)
	var have := int(o.get("have", 0))
	var need := int(o.get("need", 0))
	var done := bool(o.get("complete", false))
	var verb := String(_OBJ_TYPE_LABEL.get(int(o.get("type", 0)), "Do"))
	l.text = "%s %s: %d/%d" % ["✓" if done else "•", verb, have, need]
	l.add_theme_color_override("font_color",
		Color(0.6, 0.95, 0.5) if done else Color(0.92, 0.92, 0.96))
	return l
