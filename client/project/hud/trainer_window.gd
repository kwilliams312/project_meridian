# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the TRAINER WINDOW view (NPC-02, #372/#441). A panel that opens from
# the gossip "Train abilities" entry and renders the trainer's per-player ability list:
# each TRAINER_LIST row is an ability (id × cost in copper) with its class/level gate and
# the player's learn eligibility. A LEARNABLE row gets a Learn button; ineligible rows are
# greyed with the reason. Learning is server-authoritative (Principle 1):
#   GOSSIP_HELLO → (server pushes) TRAINER_LIST → TRAINER_LEARN → TRAINER_LEARN_RESULT.
#
# The TRAINER_LIST is PUSHED by worldd alongside the GOSSIP_MENU when the player opens
# gossip on a trainer NPC — there is no client-side "request trainer list" (the wire has
# no such opcode). So the window binds to the list as it arrives and reveals itself only
# when the gossip trainer entry is picked (bus.trainer_opened).
#
# PURE VIEW / MVVM (the #431/#433 contract): owns NO server state, never touches the net
# thread. Bound by the HUD to MeridianEventBus.trainer_list_changed / trainer_opened /
# trainer_learn_result; a Learn press becomes a bus INTENT (request_trainer_learn).
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianTrainerWindow
extends PanelContainer

const Bus := preload("res://hud/event_bus.gd")
const Money := preload("res://hud/money.gd")

const WIN_W := 340.0

var _bus: MeridianEventBus
var _title: Label
var _body: VBoxContainer      # the ability rows (rebuilt per list)
var _notice: Label
var _npc: int = 0
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.trainer_list_changed.connect(_on_trainer_list_changed)
	_bus.trainer_opened.connect(_on_trainer_opened)
	_bus.trainer_learn_result.connect(_on_trainer_learn_result)


func set_frame_visible(v: bool) -> void:
	visible = v


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(WIN_W, 0.0)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 4)
	add_child(root)

	_title = Label.new()
	_title.add_theme_color_override("font_color", Color(0.85, 0.75, 1.0))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 16)
	root.add_child(_title)

	root.add_child(HSeparator.new())

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 3)
	root.add_child(_body)

	_notice = Label.new()
	_notice.add_theme_color_override("font_color", Color(0.85, 0.55, 0.55))
	_notice.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_notice.custom_minimum_size = Vector2(WIN_W - 20.0, 0.0)
	_notice.visible = false
	root.add_child(_notice)

	var close_row := HBoxContainer.new()
	close_row.alignment = BoxContainer.ALIGNMENT_END
	var close := Button.new()
	close.text = "Close  [Esc]"
	close.pressed.connect(func(): set_frame_visible(false))
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_trainer_list_changed(npc_guid: int, entries: Array) -> void:
	# A TRAINER_LIST is pushed with the GOSSIP_MENU (before the player clicks "Train"), so
	# it must NEVER auto-open the window: only REFRESH an already-visible window for this
	# trainer. Otherwise just remember the npc so trainer_opened can render the bus's list.
	if visible and (_npc == 0 or _npc == npc_guid):
		render(npc_guid, entries)
	else:
		_npc = npc_guid


func _on_trainer_opened(npc_guid: int) -> void:
	if _bus == null:
		return
	_npc = npc_guid
	render(npc_guid, _bus.trainer_entries())


func _on_trainer_learn_result(result: Dictionary) -> void:
	var status := int(result.get("status", -1))
	if status != Bus.RESULT_OK:
		_show_notice(_learn_status_text(status))
	else:
		_show_notice("Learned ability #%d." % int(result.get("ability_id", 0)))


# --- Rendering ---------------------------------------------------------------

func render(npc_guid: int, entries: Array) -> void:
	if not _built:
		_build()
	_npc = npc_guid
	_title.text = "Trainer — NPC #%d" % npc_guid
	_clear_body()
	if entries.is_empty():
		var none := Label.new()
		none.text = "This trainer has nothing to teach you."
		none.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		_body.add_child(none)
	else:
		for e in entries:
			_add_ability_row(e as Dictionary)
	set_frame_visible(true)


func _clear_body() -> void:
	if _body == null:
		return
	for c in _body.get_children():
		c.queue_free()


func _add_ability_row(e: Dictionary) -> void:
	var ability := int(e.get("ability_id", 0))
	var cost := int(e.get("cost", 0))
	var req_class := int(e.get("required_class", 0))
	var req_level := int(e.get("required_level", 0))
	var state := int(e.get("state", Bus.TRAIN_LEARNABLE))
	var known := bool(e.get("known", false))

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)

	var info := Label.new()
	var gate := ""
	if req_level > 0:
		gate += "  (lvl %d" % req_level
		if req_class > 0:
			gate += ", class %d" % req_class
		gate += ")"
	elif req_class > 0:
		gate += "  (class %d)" % req_class
	info.text = "Ability #%d — %s%s" % [ability, Money.format_copper(cost), gate]
	info.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(info)

	var learnable := (state == Bus.TRAIN_LEARNABLE) and not known
	if learnable:
		info.add_theme_color_override("font_color", Color(1.0, 1.0, 1.0))
		var b := Button.new()
		b.text = "Learn"
		b.pressed.connect(func(): _bus.request_trainer_learn(_npc, ability) if _bus != null else null)
		row.add_child(b)
	else:
		info.add_theme_color_override("font_color", Color(0.55, 0.55, 0.6))
		var why := Label.new()
		why.text = _state_tag(state, known)
		why.add_theme_color_override("font_color", Color(0.6, 0.5, 0.5))
		row.add_child(why)
	_body.add_child(row)


# A short reason tag for an ineligible/known row (mirrors world.fbs TrainableState).
func _state_tag(state: int, known: bool) -> String:
	if known or state == Bus.TRAIN_ALREADY_KNOWN:
		return "known"
	match state:
		Bus.TRAIN_WRONG_CLASS: return "wrong class"
		Bus.TRAIN_LEVEL_TOO_LOW: return "level too low"
		Bus.TRAIN_CANT_AFFORD: return "can't afford"
		_: return "unavailable"


# world.fbs TrainerLearnStatus → a readable reason for a failed learn.
func _learn_status_text(status: int) -> String:
	match status:
		1: return "That NPC is not a trainer."
		2: return "This trainer does not teach that ability."
		3: return "Your class cannot learn that ability."
		4: return "Your level is too low to learn that."
		5: return "You already know that ability."
		6: return "You cannot afford that ability."
		_: return "You could not learn that."


func _show_notice(text: String) -> void:
	if _notice == null:
		return
	_notice.text = text
	_notice.visible = not text.is_empty()
