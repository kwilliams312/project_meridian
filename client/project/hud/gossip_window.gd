# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the GOSSIP WINDOW view (NPC-01/02, QST-01, #372/#433). A panel
# that opens on a targeted NPC and renders the server's state-gated gossip menu: each
# GOSSIP_MENU option becomes a row — an available quest to accept, an in-progress quest
# (info), a turn-in-ready quest, or a vendor / trainer ENTRY (the vendor/trainer windows
# themselves are the sibling #440; here we only fire the entry hooks).
#
# PURE VIEW / MVVM (the #431 contract): this node owns NO server state and never touches
# the net thread. The HUD (the ViewModel) subscribes it to MeridianEventBus.gossip_menu_
# changed / gossip_closed and calls show_menu() / hide_menu(); a button press turns into
# a bus INTENT (bus.request_quest_accept / request_quest_turn_in / request_vendor_entry /
# request_trainer_entry) — the world scene owns the net thread and sends the frame. So
# the server stays authoritative: the client only asks; it never predicts quest state.
#
# Built in code (like unit_frame.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianGossipWindow
extends PanelContainer

# world.fbs GossipOptionKind ordinals, re-exposed via the bus constants.
const Bus := preload("res://hud/event_bus.gd")

const WIN_W := 320.0

var _bus: MeridianEventBus
var _title: Label
var _body: VBoxContainer      # the option rows (rebuilt per menu)
var _npc_guid: int = 0
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


# Bind to the world session's event bus. Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.gossip_menu_changed.connect(_on_gossip_menu_changed)
	_bus.gossip_closed.connect(_on_gossip_closed)


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
	_title.add_theme_color_override("font_color", Color(1.0, 0.92, 0.6))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 16)
	root.add_child(_title)

	var sep := HSeparator.new()
	root.add_child(sep)

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 3)
	root.add_child(_body)

	var close_row := HBoxContainer.new()
	close_row.alignment = BoxContainer.ALIGNMENT_END
	var close := Button.new()
	close.text = "Close  [Esc]"
	close.pressed.connect(_on_close_pressed)
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_gossip_menu_changed(npc_guid: int, options: Array) -> void:
	show_menu(npc_guid, options)


func _on_gossip_closed() -> void:
	hide_menu()


# --- Rendering ---------------------------------------------------------------

func show_menu(npc_guid: int, options: Array) -> void:
	if not _built:
		_build()
	_npc_guid = npc_guid
	_title.text = "Talking to NPC #%d" % npc_guid
	_clear_body()
	if options.is_empty():
		var none := Label.new()
		none.text = "They have nothing for you right now."
		none.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		_body.add_child(none)
	else:
		for opt in options:
			_add_option_row(opt as Dictionary)
	set_frame_visible(true)


func hide_menu() -> void:
	set_frame_visible(false)
	_npc_guid = 0
	_clear_body()


func _clear_body() -> void:
	if _body == null:
		return
	for c in _body.get_children():
		c.queue_free()


func _add_option_row(opt: Dictionary) -> void:
	var kind := int(opt.get("kind", -1))
	var target_id := int(opt.get("target_id", 0))
	match kind:
		Bus.GOSSIP_QUEST_AVAILABLE:
			var b := _make_button("[!]  Accept quest #%d" % target_id,
				Color(1.0, 0.85, 0.2))
			b.pressed.connect(func(): _bus.request_quest_accept(target_id, _npc_guid))
			_body.add_child(b)
		Bus.GOSSIP_QUEST_IN_PROGRESS:
			var l := Label.new()
			l.text = "[…]  Quest #%d — in progress" % target_id
			l.add_theme_color_override("font_color", Color(0.75, 0.78, 0.85))
			_body.add_child(l)
		Bus.GOSSIP_QUEST_COMPLETE:
			var b := _make_button("[?]  Turn in quest #%d" % target_id,
				Color(0.55, 0.9, 0.4))
			# choice_index -1 = no reward choice. A choice-reward quest returns
			# BAD_CHOICE and the typed result surfaces it (reward data is not on the
			# gossip wire — a reward-picker is a follow-up once QUEST_TURN_IN carries it).
			b.pressed.connect(func(): _bus.request_quest_turn_in(target_id, _npc_guid, -1))
			_body.add_child(b)
		Bus.GOSSIP_VENDOR:
			var b := _make_button("Browse goods  (vendor)", Color(0.7, 0.85, 1.0))
			b.pressed.connect(func(): _bus.request_vendor_entry(_npc_guid))  # #440 hook
			_body.add_child(b)
		Bus.GOSSIP_TRAINER:
			var b := _make_button("Train abilities  (trainer)", Color(0.85, 0.75, 1.0))
			b.pressed.connect(func(): _bus.request_trainer_entry(_npc_guid))  # #440 hook
			_body.add_child(b)
		_:
			pass  # unknown option kind — ignore forward-compatibly


func _make_button(text: String, color: Color) -> Button:
	var b := Button.new()
	b.text = text
	b.alignment = HORIZONTAL_ALIGNMENT_LEFT
	b.add_theme_color_override("font_color", color)
	return b


func _on_close_pressed() -> void:
	if _bus != null:
		_bus.close_gossip()
	else:
		hide_menu()
