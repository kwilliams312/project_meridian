# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the LOOT WINDOW view (ITM-02, #369/#441). A panel that opens on a
# corpse and renders the server's per-looter loot session: the money pile + each lootable
# slot (item_template_id × count, coloured by rarity), a take button per row, and a
# close/release button. The flow is entirely server-authoritative (Principle 1):
#   LOOT_REQUEST (open) → LOOT_RESPONSE (money + slots) → LOOT_TAKE (a slot / the money)
#   → LOOT_RESULT (the outcome; the taken row disappears) → LOOT_RELEASE / LOOT_CLOSED.
#
# PURE VIEW / MVVM (the #431/#433 contract): this node owns NO server state and never
# touches the net thread. The HUD subscribes it to MeridianEventBus.loot_window_changed /
# loot_result / loot_closed; a take/close press becomes a bus INTENT (request_loot_take /
# request_loot_release) — the world scene owns the net thread and sends the frame.
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianLootWindow
extends PanelContainer

const Bus := preload("res://hud/event_bus.gd")
const Money := preload("res://hud/money.gd")

const WIN_W := 300.0

# Rarity tier (item.schema.yaml) → colour, mirroring the classic quality palette so a
# looter reads value at a glance. 0=poor … 5=legendary; unknown tiers fall back to common.
const _RARITY_COLORS := {
	0: Color(0.62, 0.62, 0.62),   # poor (grey)
	1: Color(1.0, 1.0, 1.0),      # common (white)
	2: Color(0.12, 1.0, 0.0),     # uncommon (green)
	3: Color(0.0, 0.44, 0.87),    # rare (blue)
	4: Color(0.64, 0.21, 0.93),   # epic (purple)
	5: Color(1.0, 0.5, 0.0),      # legendary (orange)
}

# world.fbs LootStatus rejection reasons (non-OK LOOT_RESPONSE) for a readable notice.
const _LOOT_STATUS_TEXT := {
	1: "You are not an eligible looter of this corpse.",
	2: "You are too far from the corpse to loot.",
	3: "There is no corpse to loot.",
	4: "This corpse has already been looted.",
}

var _bus: MeridianEventBus
var _title: Label
var _money_btn: Button
var _body: VBoxContainer      # the slot rows (rebuilt per response)
var _notice: Label
var _corpse: int = 0
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


# Bind to the world session's event bus. Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.loot_window_changed.connect(_on_loot_window_changed)
	_bus.loot_result.connect(_on_loot_result)
	_bus.loot_closed.connect(_on_loot_closed)


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
	_title.add_theme_color_override("font_color", Color(0.85, 0.7, 0.4))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 16)
	root.add_child(_title)

	root.add_child(HSeparator.new())

	# The money pile (a take button; hidden when the corpse has no money).
	_money_btn = Button.new()
	_money_btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_money_btn.add_theme_color_override("font_color", Color(1.0, 0.85, 0.2))
	_money_btn.pressed.connect(_on_money_pressed)
	root.add_child(_money_btn)

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
	close.pressed.connect(_on_close_pressed)
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_loot_window_changed(corpse_guid: int, status: int, copper: int, items: Array) -> void:
	render(corpse_guid, status, copper, items)


func _on_loot_result(result: Dictionary) -> void:
	# A typed rejection surfaces its reason; an OK take is already reflected by the
	# follow-up loot_window_changed (the taken row was removed). Loot-out auto-closes.
	var status := int(result.get("status", -1))
	if status != Bus.LOOT_OK:
		_show_notice(_take_status_text(status))


func _on_loot_closed(corpse_guid: int) -> void:
	if corpse_guid == _corpse or _corpse == 0:
		hide_window()


# --- Rendering ---------------------------------------------------------------

func render(corpse_guid: int, status: int, copper: int, items: Array) -> void:
	if not _built:
		_build()
	_corpse = corpse_guid
	_title.text = "Loot — corpse #%d" % corpse_guid
	_clear_body()
	if status != Bus.LOOT_OK:
		_money_btn.visible = false
		_show_notice(String(_LOOT_STATUS_TEXT.get(status, "You cannot loot this.")))
		set_frame_visible(true)
		return
	_notice.visible = false

	# Money pile.
	_money_btn.visible = copper > 0
	if copper > 0:
		_money_btn.text = "Take money:  %s" % Money.format_copper(copper)

	# Item slots.
	if items.is_empty() and copper <= 0:
		var none := Label.new()
		none.text = "Empty."
		none.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		_body.add_child(none)
	else:
		for it in items:
			_add_item_row(it as Dictionary)
	set_frame_visible(true)


func hide_window() -> void:
	set_frame_visible(false)
	_corpse = 0
	_clear_body()
	_notice.visible = false


func _clear_body() -> void:
	if _body == null:
		return
	for c in _body.get_children():
		c.queue_free()


func _add_item_row(it: Dictionary) -> void:
	var slot := int(it.get("slot", 0))
	var tmpl := int(it.get("item_template_id", 0))
	var count := int(it.get("count", 1))
	var quality := int(it.get("quality", 1))
	var quest_item := bool(it.get("quest_item", false))
	var b := Button.new()
	b.alignment = HORIZONTAL_ALIGNMENT_LEFT
	var label := "Item #%d" % tmpl
	if count > 1:
		label += "  x%d" % count
	if quest_item:
		label = "[quest]  " + label
	b.text = label
	b.add_theme_color_override("font_color",
		_RARITY_COLORS.get(quality, _RARITY_COLORS[1]))
	b.pressed.connect(func(): _bus.request_loot_take(_corpse, slot, false) if _bus != null else null)
	_body.add_child(b)


func _show_notice(text: String) -> void:
	if _notice == null:
		return
	_notice.text = text
	_notice.visible = not text.is_empty()


# world.fbs LootTakeStatus → a readable reason for a failed take.
func _take_status_text(status: int) -> String:
	match status:
		1: return "You are not an eligible looter."
		2: return "You are too far from the corpse."
		3: return "That was already taken."
		4: return "That is a quest item you are not on the quest for."
		5: return "Your inventory is full."
		6: return "No such slot on this corpse."
		7: return "There is no corpse to loot."
		_: return "You could not take that."


func _on_money_pressed() -> void:
	if _bus != null and _corpse != 0:
		_bus.request_loot_take(_corpse, 0, true)  # money = true


func _on_close_pressed() -> void:
	if _bus != null and _corpse != 0:
		_bus.request_loot_release(_corpse)
	else:
		hide_window()
