# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the BAGS / INVENTORY WINDOW view (ITM-01, #441/#471). The character's
# real backpack contents + copper balance, fed by the server-authoritative INVENTORY_SNAPSHOT
# (0x5007): worldd pushes it at ENTER_WORLD and re-pushes after every inventory change
# (loot take, vendor buy/sell/buyback, quest reward, GM .additem), so the grid + money always
# reflect durable server state. The client NEVER predicts its bags (Principle 1, "server is
# law") — it renders exactly what the snapshot carried.
#
# Each row shows the backpack slot, item template id × count, coloured by rarity (the same
# palette the loot window uses). `money` rides the snapshot too, so the balance is known from
# spawn (no longer "unknown until a transaction").
#
# PURE VIEW / MVVM (the #431/#433 contract): owns NO server state, never touches the net
# thread. Bound by the HUD to MeridianEventBus.inventory_changed / currency_changed; toggled
# by a HUD key.
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianBagsWindow
extends PanelContainer

const Money := preload("res://hud/money.gd")

const WIN_W := 420.0

# Rarity tier (item.schema.yaml) → colour, mirroring the loot window's palette so a player
# reads value at a glance. 0=poor … 5=legendary; unknown tiers fall back to common (white).
const _RARITY_COLORS := {
	0: Color(0.62, 0.62, 0.62),   # poor (grey)
	1: Color(1.0, 1.0, 1.0),      # common (white)
	2: Color(0.12, 1.0, 0.0),     # uncommon (green)
	3: Color(0.0, 0.44, 0.87),    # rare (blue)
	4: Color(0.64, 0.21, 0.93),   # epic (purple)
	5: Color(1.0, 0.5, 0.0),      # legendary (orange)
}

var _bus: MeridianEventBus
var _money_label: Label
var _slots_label: Label
var _item_list: VBoxContainer
var _equipment_list: VBoxContainer
var _status_label: Label
var _pending := false
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.inventory_changed.connect(_on_inventory_changed)
	_bus.equipment_changed.connect(_on_equipment_changed)
	_bus.equipment_change_result.connect(_on_equipment_change_result)
	_bus.currency_changed.connect(_on_currency_changed)
	_render_money()
	_render_items()


func set_frame_visible(v: bool) -> void:
	visible = v


# Toggle visibility (bound to a HUD key by the world scene). Refreshes on show.
func toggle() -> void:
	if not _built:
		_build()
	set_frame_visible(not visible)
	if visible:
		_render_money()
		_render_items()


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(WIN_W, 0.0)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 4)
	add_child(root)

	var title := Label.new()
	title.text = "Bags"
	title.add_theme_color_override("font_color", Color(0.85, 0.8, 0.55))
	title.add_theme_color_override("font_outline_color", Color.BLACK)
	title.add_theme_constant_override("outline_size", 4)
	title.add_theme_font_size_override("font_size", 16)
	root.add_child(title)

	root.add_child(HSeparator.new())

	_money_label = Label.new()
	_money_label.add_theme_color_override("font_color", Color(1.0, 0.85, 0.2))
	root.add_child(_money_label)

	_slots_label = Label.new()
	_slots_label.add_theme_color_override("font_color", Color(0.7, 0.72, 0.78))
	_slots_label.add_theme_font_size_override("font_size", 11)
	root.add_child(_slots_label)

	root.add_child(HSeparator.new())
	var equipment_title := Label.new()
	equipment_title.text = "Equipped"
	equipment_title.add_theme_font_size_override("font_size", 13)
	root.add_child(equipment_title)
	_equipment_list = VBoxContainer.new()
	_equipment_list.add_theme_constant_override("separation", 4)
	root.add_child(_equipment_list)
	root.add_child(HSeparator.new())
	var backpack_title := Label.new()
	backpack_title.text = "Backpack"
	backpack_title.add_theme_font_size_override("font_size", 13)
	root.add_child(backpack_title)

	# The real backpack rows (one per occupied slot), rebuilt on each INVENTORY_SNAPSHOT.
	_item_list = VBoxContainer.new()
	_item_list.add_theme_constant_override("separation", 2)
	root.add_child(_item_list)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.add_theme_color_override("font_color", Color(0.78, 0.8, 0.86))
	_status_label.add_theme_font_size_override("font_size", 11)
	root.add_child(_status_label)

	var close_row := HBoxContainer.new()
	close_row.alignment = BoxContainer.ALIGNMENT_END
	var close := Button.new()
	close.text = "Close  [B]"
	close.pressed.connect(func(): set_frame_visible(false))
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true
	_render_money()
	_render_items()


func _on_inventory_changed(_money: int, _items: Array, _backpack_slots: int) -> void:
	_pending = false
	_render_money()
	_render_items()


func _on_equipment_changed(_equipment: Array) -> void:
	_render_equipment()


func _on_equipment_change_result(result: Dictionary) -> void:
	var status := int(result.get("status", -1))
	_status_label.text = _equipment_status_text(status)
	if status != 0:
		_pending = false
		_render_items()
		_render_equipment()


func _on_currency_changed(_copper: int, _known: bool) -> void:
	_render_money()


func _render_money() -> void:
	if _money_label == null:
		return
	if _bus != null and _bus.currency_known():
		_money_label.text = "Money: %s" % Money.format_copper(_bus.copper())
	else:
		_money_label.text = "Money: (unknown until snapshot)"


func _render_items() -> void:
	if _item_list == null:
		return
	for c in _item_list.get_children():
		c.queue_free()

	if _bus == null or not _bus.inventory_known():
		var pending := Label.new()
		pending.text = "Waiting for inventory snapshot…"
		pending.add_theme_color_override("font_color", Color(0.7, 0.72, 0.78))
		pending.add_theme_font_size_override("font_size", 11)
		_item_list.add_child(pending)
		_render_slots(0, 0)
		return

	var items := _bus.inventory_items()
	_render_slots(items.size(), _bus.backpack_slots())

	if items.is_empty():
		var empty := Label.new()
		empty.text = "Your bags are empty."
		empty.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		_item_list.add_child(empty)
		return

	for it in items:
		_add_item_row(it as Dictionary)
	_render_equipment()


func _add_item_row(it: Dictionary) -> void:
	var slot := int(it.get("slot", 0))
	var tmpl := int(it.get("item_template_id", 0))
	var count := int(it.get("count", 1))
	var quality := int(it.get("quality", 1))
	var bound := int(it.get("binding", 0)) != 0

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	var label := Label.new()
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var name_text := "Item #%d" % tmpl
	if count > 1:
		name_text += " x%d" % count
	if bound:
		name_text += "  (bound)"
	label.text = "[%d] %s" % [slot, name_text]
	label.add_theme_color_override("font_color",
		_RARITY_COLORS.get(quality, _RARITY_COLORS[1]))
	label.add_theme_color_override("font_outline_color", Color.BLACK)
	label.add_theme_constant_override("outline_size", 3)
	row.add_child(label)
	var equip := Button.new()
	equip.text = "Equip"
	equip.disabled = _pending
	equip.tooltip_text = "Equip Item #%d from backpack slot %d" % [tmpl, slot]
	equip.accessibility_name = "Equip Item %d from backpack slot %d" % [tmpl, slot]
	equip.accessibility_description = "Moves this item to its server-approved equipment slot."
	equip.pressed.connect(func(): _request_equipment_change(0, slot))
	row.add_child(equip)
	_item_list.add_child(row)


func _render_equipment() -> void:
	if _equipment_list == null:
		return
	for c in _equipment_list.get_children():
		c.queue_free()
	if _bus == null or not _bus.inventory_known():
		var waiting := Label.new()
		waiting.text = "Waiting for equipment snapshot…"
		_equipment_list.add_child(waiting)
		return
	var equipment := _bus.equipment_items()
	if equipment.is_empty():
		var empty := Label.new()
		empty.text = "No items equipped."
		_equipment_list.add_child(empty)
		return
	for entry in equipment:
		var it := entry as Dictionary
		var slot := int(it.get("slot", 0))
		var tmpl := int(it.get("item_template_id", 0))
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 8)
		var label := Label.new()
		label.text = "Slot %d — Item #%d" % [slot, tmpl]
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label.add_theme_color_override("font_color",
			_RARITY_COLORS.get(int(it.get("quality", 1)), _RARITY_COLORS[1]))
		row.add_child(label)
		var unequip := Button.new()
		unequip.text = "Unequip"
		unequip.disabled = _pending
		unequip.tooltip_text = "Move Item #%d from equipment slot %d to your backpack" % [tmpl, slot]
		unequip.accessibility_name = "Unequip Item %d from equipment slot %d" % [tmpl, slot]
		unequip.accessibility_description = "Moves this item to the first available backpack slot."
		unequip.pressed.connect(func(): _request_equipment_change(1, slot))
		row.add_child(unequip)
		_equipment_list.add_child(row)


func _request_equipment_change(action: int, slot: int) -> void:
	if _bus == null or _pending:
		return
	_pending = true
	_status_label.text = "Updating equipment…"
	_render_items()
	_render_equipment()
	if action == 0:
		_bus.request_equip(slot)
	else:
		_bus.request_unequip(slot)


func _equipment_status_text(status: int) -> String:
	const REASONS := {
		0: "Equipment updated.", 1: "Enter the world before changing equipment.",
		2: "That slot is invalid.", 3: "That slot is empty.",
		4: "That item cannot be equipped.", 5: "Your level is too low for that item.",
		6: "Your class is not proficient with that item.",
		7: "That item's category does not match its equipment slot.",
		8: "That item's equipment type is unknown.",
		9: "Resolve the two-hand/off-hand conflict first.",
		10: "Your backpack is full.", 11: "Your character class is unknown.",
		12: "The server could not update equipment. Try again.",
	}
	return String(REASONS.get(status, "Equipment update was rejected."))


func _render_slots(used: int, capacity: int) -> void:
	if _slots_label == null:
		return
	if capacity > 0:
		_slots_label.text = "Slots: %d / %d used" % [used, capacity]
	else:
		_slots_label.text = ""
