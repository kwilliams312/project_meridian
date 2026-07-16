# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the VENDOR WINDOW view (ECO-01, #370/#441/#471). A panel that opens from
# the gossip "Browse goods" entry and drives the server-authoritative vendor economy: buy,
# sell, and a buyback tab. Every price + balance is the SERVER's (Principle 1) — a request
# NEVER carries a client-trusted price:
#   VENDOR_BUY_REQUEST  → VENDOR_BUY_RESULT      (minted item + debited copper + balance)
#   VENDOR_SELL_REQUEST → VENDOR_SELL_RESULT     (credited copper + a buyback slot + balance)
#   VENDOR_BUYBACK_REQUEST → VENDOR_BUYBACK_RESULT (re-minted item + re-debited + balance)
#
# The BUY tab renders the REAL catalog (VENDOR_LIST, 0x5107 — auto-pushed on GOSSIP_HELLO to a
# vendor NPC, like TRAINER_LIST): each row is a listed item with its SERVER-COMPUTED price +
# stock. The buy path still re-validates every price server-side (NOT_SOLD / UNKNOWN_VENDOR).
# The BUYBACK tab is likewise fully wire-populated (each VENDOR_SELL_RESULT pushes an entry the
# player can repurchase for exactly what they sold it for); a VENDOR_BUYBACK_RESULT echoes its
# slot so the row drops without client-side correlation.
#
# The SELL control is backpack-slot-driven: the real slot numbers now show in the Bags window
# (INVENTORY_SNAPSHOT, #471); the server validates the slot (SLOT_EMPTY / NOT_SELLABLE).
#
# PURE VIEW / MVVM (the #431/#433 contract): owns NO server state, never touches the net
# thread. Bound by the HUD to vendor_opened / vendor_catalog_changed / vendor_result /
# vendor_buyback_changed / currency_changed; a buy/sell/buyback press becomes a bus INTENT.
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianVendorWindow
extends PanelContainer

const Bus := preload("res://hud/event_bus.gd")
const Money := preload("res://hud/money.gd")

const WIN_W := 360.0

# Rarity tier (item.schema.yaml) → colour, mirroring the loot/bags palette so a shopper reads
# value at a glance. 0=poor … 5=legendary; unknown tiers fall back to common (white).
const _RARITY_COLORS := {
	0: Color(0.62, 0.62, 0.62),   # poor (grey)
	1: Color(1.0, 1.0, 1.0),      # common (white)
	2: Color(0.12, 1.0, 0.0),     # uncommon (green)
	3: Color(0.0, 0.44, 0.87),    # rare (blue)
	4: Color(0.64, 0.21, 0.93),   # epic (purple)
	5: Color(1.0, 0.5, 0.0),      # legendary (orange)
}

var _bus: MeridianEventBus
var _title: Label
var _balance_label: Label
var _notice: Label
var _buy_qty: SpinBox
var _catalog_list: VBoxContainer
var _sell_slot: SpinBox
var _sell_qty: SpinBox
var _buyback_list: VBoxContainer
var _vendor_id: int = 0
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.vendor_opened.connect(_on_vendor_opened)
	_bus.vendor_closed.connect(_on_vendor_closed)
	_bus.vendor_catalog_changed.connect(_on_catalog_changed)
	_bus.vendor_result.connect(_on_vendor_result)
	_bus.vendor_buyback_changed.connect(_on_buyback_changed)
	_bus.currency_changed.connect(_on_currency_changed)


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
	_title.add_theme_color_override("font_color", Color(0.7, 0.85, 1.0))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 16)
	root.add_child(_title)

	_balance_label = Label.new()
	_balance_label.add_theme_color_override("font_color", Color(1.0, 0.85, 0.2))
	root.add_child(_balance_label)

	root.add_child(HSeparator.new())

	var tabs := TabContainer.new()
	tabs.custom_minimum_size = Vector2(WIN_W - 12.0, 220.0)
	root.add_child(tabs)

	# --- Buy tab (the real VENDOR_LIST catalog) ------------------------------
	var buy := VBoxContainer.new()
	buy.name = "Buy"
	buy.add_theme_constant_override("separation", 4)
	tabs.add_child(buy)

	var qty_row := HBoxContainer.new()
	qty_row.add_theme_constant_override("separation", 4)
	_buy_qty = _make_spin(1, 1000, 1)
	qty_row.add_child(_labeled("Buy quantity", _buy_qty))
	buy.add_child(qty_row)

	var catalog_scroll := ScrollContainer.new()
	catalog_scroll.custom_minimum_size = Vector2(WIN_W - 24.0, 150.0)
	catalog_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	buy.add_child(catalog_scroll)
	_catalog_list = VBoxContainer.new()
	_catalog_list.add_theme_constant_override("separation", 2)
	_catalog_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	catalog_scroll.add_child(_catalog_list)

	# --- Sell tab ------------------------------------------------------------
	var sell := VBoxContainer.new()
	sell.name = "Sell"
	sell.add_theme_constant_override("separation", 4)
	tabs.add_child(sell)

	var sell_hint := Label.new()
	sell_hint.text = "Sell by backpack slot (slot numbers show in the Bags window):"
	sell_hint.add_theme_color_override("font_color", Color(0.75, 0.78, 0.85))
	sell_hint.add_theme_font_size_override("font_size", 11)
	sell_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	sell_hint.custom_minimum_size = Vector2(WIN_W - 24.0, 0.0)
	sell.add_child(sell_hint)
	var sell_row := HBoxContainer.new()
	sell_row.add_theme_constant_override("separation", 4)
	_sell_slot = _make_spin(0, 255, 0)
	_sell_qty = _make_spin(1, 1000, 1)
	sell_row.add_child(_labeled("slot", _sell_slot))
	sell_row.add_child(_labeled("x", _sell_qty))
	var sell_btn := Button.new()
	sell_btn.text = "Sell"
	sell_btn.pressed.connect(_on_sell_pressed)
	sell_row.add_child(sell_btn)
	sell.add_child(sell_row)

	# --- Buyback tab ---------------------------------------------------------
	var buyback := VBoxContainer.new()
	buyback.name = "Buyback"
	buyback.add_theme_constant_override("separation", 3)
	tabs.add_child(buyback)
	_buyback_list = buyback

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
	# Route the close through the bus (like the gossip window) so the open-window guid the
	# world scene tracks for the #851 out-of-range auto-close is cleared; fall back to a
	# local hide when unbound (a standalone verify with no bus).
	close.pressed.connect(_on_close_pressed)
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true
	_render_catalog([])
	_render_buyback([])
	_render_balance()


# --- Bus signal handlers -----------------------------------------------------

func _on_vendor_opened(vendor_id: int) -> void:
	if not _built:
		_build()
	_vendor_id = vendor_id
	_title.text = "Vendor #%d" % vendor_id
	_notice.visible = false
	_render_balance()
	if _bus != null:
		_render_catalog(_bus.vendor_catalog())
		_render_buyback(_bus.buyback_entries())
	set_frame_visible(true)


func _on_vendor_closed(_vendor_id_arg: int) -> void:
	set_frame_visible(false)


# The Close button: ask the bus to close (clears the world scene's open-window tracking for
# the #851 auto-close), which re-enters as _on_vendor_closed; hide directly if unbound.
func _on_close_pressed() -> void:
	if _bus != null:
		_bus.close_vendor()
	else:
		set_frame_visible(false)


func _on_catalog_changed(vendor_id: int, items: Array) -> void:
	_vendor_id = vendor_id
	_render_catalog(items)


func _on_vendor_result(result: Dictionary) -> void:
	var status := int(result.get("status", -1))
	var kind := String(result.get("kind", ""))
	if status == Bus.RESULT_OK:
		_show_notice(_ok_text(kind, result), false)
	else:
		_show_notice(_reject_text(kind, status), true)


func _on_buyback_changed(entries: Array) -> void:
	_render_buyback(entries)


func _on_currency_changed(_copper: int, _known: bool) -> void:
	_render_balance()


# --- Rendering ---------------------------------------------------------------

func _render_balance() -> void:
	if _balance_label == null or _bus == null:
		return
	if _bus.currency_known():
		_balance_label.text = "Money: %s" % Money.format_copper(_bus.copper())
	else:
		_balance_label.text = "Money: (unknown until snapshot)"


func _render_catalog(items: Array) -> void:
	if _catalog_list == null:
		return
	for c in _catalog_list.get_children():
		c.queue_free()
	if items.is_empty():
		var none := Label.new()
		none.text = "This vendor has nothing for sale."
		none.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		_catalog_list.add_child(none)
		return
	for e in items:
		var d := e as Dictionary
		var tmpl := int(d.get("item_template_id", 0))
		var price := int(d.get("price", 0))
		var quality := int(d.get("quality", 1))
		var stock := int(d.get("stock", -1))
		var stock_text := "" if stock < 0 else "  (stock: %d)" % stock
		var b := Button.new()
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.text = "Item #%d — %s%s" % [tmpl, Money.format_copper(price), stock_text]
		b.add_theme_color_override("font_color",
			_RARITY_COLORS.get(quality, _RARITY_COLORS[1]))
		b.pressed.connect(_buy_item.bind(tmpl))
		_catalog_list.add_child(b)


func _render_buyback(entries: Array) -> void:
	if _buyback_list == null:
		return
	for c in _buyback_list.get_children():
		c.queue_free()
	if entries.is_empty():
		var none := Label.new()
		none.text = "Nothing to buy back."
		none.add_theme_color_override("font_color", Color(0.8, 0.8, 0.85))
		_buyback_list.add_child(none)
		return
	for e in entries:
		var d := e as Dictionary
		var slot := int(d.get("buyback_slot", 0))
		var tmpl := int(d.get("item_template_id", 0))
		var qty := int(d.get("quantity", 1))
		var price := int(d.get("price", 0))
		var b := Button.new()
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.text = "Item #%d x%d — %s" % [tmpl, qty, Money.format_copper(price)]
		b.pressed.connect(func(): _bus.request_vendor_buyback(slot) if _bus != null else null)
		_buyback_list.add_child(b)


func _buy_item(item_template_id: int) -> void:
	if _bus == null:
		return
	_bus.request_vendor_buy(_vendor_id, item_template_id, int(_buy_qty.value))


func _on_sell_pressed() -> void:
	if _bus == null:
		return
	_bus.request_vendor_sell(_vendor_id, int(_sell_slot.value), int(_sell_qty.value))


# --- Result text -------------------------------------------------------------

func _ok_text(kind: String, result: Dictionary) -> String:
	match kind:
		"buy": return "Bought item #%d x%d for %s." % [
			int(result.get("item_template_id", 0)), int(result.get("quantity", 0)),
			Money.format_copper(int(result.get("total_price", 0)))]
		"sell": return "Sold for %s." % Money.format_copper(int(result.get("total_credit", 0)))
		"buyback": return "Bought back item #%d for %s." % [
			int(result.get("item_template_id", 0)),
			Money.format_copper(int(result.get("price", 0)))]
		_: return "Done."


func _reject_text(kind: String, status: int) -> String:
	match kind:
		"buy":
			match status:
				1: return "No such vendor."
				2: return "This vendor does not sell that."
				3: return "You cannot afford that."
				4: return "Your inventory is full."
				5: return "Invalid quantity."
				6: return "You are not in the world."
				_: return "The purchase failed."
		"sell":
			match status:
				1: return "That backpack slot is empty."
				2: return "That item cannot be sold."
				3: return "Invalid quantity."
				4: return "You are not in the world."
				_: return "The sale failed."
		"buyback":
			match status:
				1: return "That buyback entry is gone."
				2: return "You cannot afford to buy that back."
				3: return "Your inventory is full."
				4: return "You are not in the world."
				_: return "The buyback failed."
		_: return "That failed."


func _show_notice(text: String, is_error: bool) -> void:
	if _notice == null:
		return
	_notice.add_theme_color_override("font_color",
		Color(0.85, 0.55, 0.55) if is_error else Color(0.55, 0.85, 0.55))
	_notice.text = text
	_notice.visible = not text.is_empty()


# --- Small builders ----------------------------------------------------------

func _make_spin(min_v: float, max_v: float, val: float) -> SpinBox:
	var s := SpinBox.new()
	s.min_value = min_v
	s.max_value = max_v
	s.value = val
	s.step = 1
	s.custom_minimum_size = Vector2(78.0, 0.0)
	return s


func _labeled(text: String, control: Control) -> HBoxContainer:
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 2)
	var l := Label.new()
	l.text = text
	l.add_theme_color_override("font_color", Color(0.85, 0.85, 0.9))
	h.add_child(l)
	h.add_child(control)
	return h
