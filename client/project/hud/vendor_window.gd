# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the VENDOR WINDOW view (ECO-01, #370/#441). A panel that opens from
# the gossip "Browse goods" entry and drives the server-authoritative vendor economy:
# buy, sell, and a buyback tab. Every price + balance is the SERVER's (Principle 1) — a
# request NEVER carries a client-trusted price:
#   VENDOR_BUY_REQUEST  → VENDOR_BUY_RESULT      (minted item + debited copper + balance)
#   VENDOR_SELL_REQUEST → VENDOR_SELL_RESULT     (credited copper + a buyback slot + balance)
#   VENDOR_BUYBACK_REQUEST → VENDOR_BUYBACK_RESULT (re-minted item + re-debited + balance)
#
# ⛔ WIRE-CONTRACT GAPS (world.fbs has NO opcode for either — reported, not faked; see the
# gap note in event_bus.gd):
#   * NO vendor-catalog listing → the BUY control is item-template-id-driven (greybox,
#     like gossip's "quest #N"). The server validates the id (NOT_SOLD / UNKNOWN_VENDOR).
#   * NO inventory-contents snapshot → the SELL control is backpack-slot-driven. The
#     server validates the slot (SLOT_EMPTY / NOT_SELLABLE).
# The BUYBACK tab, by contrast, IS fully wire-populated: each VENDOR_SELL_RESULT pushes an
# entry (item × price × slot) the player can repurchase for exactly what they sold it for.
#
# PURE VIEW / MVVM (the #431/#433 contract): owns NO server state, never touches the net
# thread. Bound by the HUD to vendor_opened / vendor_result / vendor_buyback_changed /
# currency_changed; a buy/sell/buyback press becomes a bus INTENT.
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianVendorWindow
extends PanelContainer

const Bus := preload("res://hud/event_bus.gd")
const Money := preload("res://hud/money.gd")

const WIN_W := 360.0

var _bus: MeridianEventBus
var _title: Label
var _balance_label: Label
var _notice: Label
var _buy_tmpl: SpinBox
var _buy_qty: SpinBox
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
	tabs.custom_minimum_size = Vector2(WIN_W - 12.0, 190.0)
	root.add_child(tabs)

	# --- Buy / Sell tab ------------------------------------------------------
	var trade := VBoxContainer.new()
	trade.name = "Buy / Sell"
	trade.add_theme_constant_override("separation", 4)
	tabs.add_child(trade)

	var buy_hint := Label.new()
	buy_hint.text = "Buy by item id (no catalog on wire — see gap):"
	buy_hint.add_theme_color_override("font_color", Color(0.75, 0.78, 0.85))
	buy_hint.add_theme_font_size_override("font_size", 11)
	trade.add_child(buy_hint)
	var buy_row := HBoxContainer.new()
	buy_row.add_theme_constant_override("separation", 4)
	_buy_tmpl = _make_spin(1, 999999, 1)
	_buy_qty = _make_spin(1, 1000, 1)
	buy_row.add_child(_labeled("item #", _buy_tmpl))
	buy_row.add_child(_labeled("x", _buy_qty))
	var buy_btn := Button.new()
	buy_btn.text = "Buy"
	buy_btn.pressed.connect(_on_buy_pressed)
	buy_row.add_child(buy_btn)
	trade.add_child(buy_row)

	trade.add_child(HSeparator.new())

	var sell_hint := Label.new()
	sell_hint.text = "Sell by backpack slot (no inventory on wire — see gap):"
	sell_hint.add_theme_color_override("font_color", Color(0.75, 0.78, 0.85))
	sell_hint.add_theme_font_size_override("font_size", 11)
	trade.add_child(sell_hint)
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
	trade.add_child(sell_row)

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
	close.pressed.connect(func(): set_frame_visible(false))
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true
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
		_render_buyback(_bus.buyback_entries())
	set_frame_visible(true)


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
		_balance_label.text = "Money: (unknown until a transaction — no wire snapshot)"


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


func _on_buy_pressed() -> void:
	if _bus == null:
		return
	_bus.request_vendor_buy(_vendor_id, int(_buy_tmpl.value), int(_buy_qty.value))


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
