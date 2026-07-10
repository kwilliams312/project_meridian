# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the QUEST LOG WINDOW view (QST-01, #433). A toggled panel listing
# the character's active quests, each with its level, its objectives (have/need +
# complete tick), and a turn-in-ready cue. Clicking a quest WATCHES it (drives the
# on-HUD tracker).
#
# PURE VIEW / MVVM (#431 contract): owns no server state, never touches the net thread.
# The HUD subscribes it to MeridianEventBus.quest_log_changed / tracked_quest_changed
# and calls render(); a click emits the bus intent set_tracked_quest(). The log data is
# the server's authoritative QUEST_LOG snapshot — the client only displays it.
#
# Built in code (like unit_frame.gd) — self-contained, no .tscn.
class_name MeridianQuestLogWindow
extends PanelContainer

const Bus := preload("res://hud/event_bus.gd")

const WIN_W := 340.0
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
	_bus.quest_log_changed.connect(_on_quest_log_changed)
	_bus.tracked_quest_changed.connect(_on_tracked_changed)
	_bus.quest_progress_changed.connect(_on_progress_changed)
	render()


func set_frame_visible(v: bool) -> void:
	visible = v


## Toggle visibility. On show, re-render from the bus so the log is current.
func toggle() -> void:
	set_frame_visible(not visible)
	if visible:
		render()


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(WIN_W, 0.0)
	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 4)
	add_child(root)

	_title = Label.new()
	_title.add_theme_color_override("font_color", Color(1.0, 0.92, 0.6))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 16)
	root.add_child(_title)
	root.add_child(HSeparator.new())

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 6)
	root.add_child(_body)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_quest_log_changed(_quests: Array) -> void:
	render()


func _on_tracked_changed(_quest_id: int) -> void:
	render()  # re-highlight the watched quest


func _on_progress_changed(_q: int, _i: int, _h: int, _n: int, _c: bool) -> void:
	if visible:
		render()


# --- Rendering ---------------------------------------------------------------

func render() -> void:
	if not _built or _bus == null:
		return
	var quests := _bus.quest_log()
	var tracked := _bus.tracked_quest()
	_title.text = "Quest Log  (%d)" % quests.size()
	for c in _body.get_children():
		c.queue_free()
	if quests.is_empty():
		var none := Label.new()
		none.text = "No active quests. Talk to an NPC with a [!] to pick one up."
		none.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		none.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		none.custom_minimum_size = Vector2(WIN_W - 16.0, 0.0)
		_body.add_child(none)
		return
	for q in quests:
		_add_quest_block(q as Dictionary, tracked)


func _add_quest_block(q: Dictionary, tracked: int) -> void:
	var quest_id := int(q.get("quest_id", 0))
	var level := int(q.get("level", 0))
	var complete := bool(q.get("complete", false))
	var is_tracked := quest_id == tracked

	# The quest header is a button so clicking it WATCHES the quest (tracker follows).
	var header := Button.new()
	header.alignment = HORIZONTAL_ALIGNMENT_LEFT
	var prefix := "▶ " if is_tracked else "   "
	var suffix := "   ✓ ready to turn in" if complete else ""
	header.text = "%sQuest #%d  (Lv %d)%s" % [prefix, quest_id, level, suffix]
	header.add_theme_color_override("font_color",
		Color(0.55, 0.95, 0.45) if complete else (
			Color(1.0, 0.9, 0.5) if is_tracked else Color(0.9, 0.9, 0.95)))
	header.pressed.connect(func(): _bus.set_tracked_quest(quest_id))
	_body.add_child(header)

	for o in q.get("objectives", []):
		_body.add_child(_objective_line(o as Dictionary))


func _objective_line(o: Dictionary) -> Label:
	var l := Label.new()
	var otype := int(o.get("type", 0))
	var have := int(o.get("have", 0))
	var need := int(o.get("need", 0))
	var done := bool(o.get("complete", false))
	var verb := String(_OBJ_TYPE_LABEL.get(otype, "Do"))
	var tick := "✓" if done else " "
	l.text = "    [%s]  %s #%d: %d / %d" % [tick, verb, int(o.get("target_id", 0)), have, need]
	l.add_theme_color_override("font_color",
		Color(0.55, 0.9, 0.45) if done else Color(0.82, 0.84, 0.9))
	return l
