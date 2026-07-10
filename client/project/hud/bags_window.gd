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

const WIN_W := 300.0

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
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.inventory_changed.connect(_on_inventory_changed)
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

	# The real backpack rows (one per occupied slot), rebuilt on each INVENTORY_SNAPSHOT.
	_item_list = VBoxContainer.new()
	_item_list.add_theme_constant_override("separation", 2)
	root.add_child(_item_list)

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
	_render_money()
	_render_items()


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


func _add_item_row(it: Dictionary) -> void:
	var slot := int(it.get("slot", 0))
	var tmpl := int(it.get("item_template_id", 0))
	var count := int(it.get("count", 1))
	var quality := int(it.get("quality", 1))
	var bound := int(it.get("binding", 0)) != 0

	var row := Label.new()
	var name_text := "Item #%d" % tmpl
	if count > 1:
		name_text += " x%d" % count
	if bound:
		name_text += "  (bound)"
	row.text = "[%d] %s" % [slot, name_text]
	row.add_theme_color_override("font_color",
		_RARITY_COLORS.get(quality, _RARITY_COLORS[1]))
	row.add_theme_color_override("font_outline_color", Color.BLACK)
	row.add_theme_constant_override("outline_size", 3)
	_item_list.add_child(row)


func _render_slots(used: int, capacity: int) -> void:
	if _slots_label == null:
		return
	if capacity > 0:
		_slots_label.text = "Slots: %d / %d used" % [used, capacity]
	else:
		_slots_label.text = ""
