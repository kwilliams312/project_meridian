# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the BAGS / INVENTORY WINDOW view (ITM-01, #441). The shared surface
# the loot / vendor / quest-reward flows feed into: the character's items + copper.
#
# ⛔ WIRE-CONTRACT GAP (reported like #439 self-vitals / #443 quest-reward-preview, NOT
# faked): world.fbs ships NO inventory-contents opcode — worldd never sends the character's
# item list to the client. So this window renders the ONE inventory-adjacent piece of state
# the wire DOES carry authoritatively — the COPPER balance, surfaced by every transaction
# result's `balance` / `new_balance` (VENDOR_BUY/SELL/BUYBACK_RESULT, TRAINER_LEARN_RESULT)
# — and shows an explicit notice that the item grid is blocked on a future INVENTORY_LIST
# contract. It deliberately does NOT fabricate an item list client-side (which would drift
# from server truth: sells/loot-takes/buys mutate the backpack the client cannot see).
#
# There is also no spawn-time money snapshot on the wire, so the balance reads "unknown"
# until the first balance-carrying transaction result arrives.
#
# PURE VIEW / MVVM (the #431/#433 contract): owns NO server state, never touches the net
# thread. Bound by the HUD to MeridianEventBus.currency_changed; toggled by a HUD key.
#
# Built in code (like gossip_window.gd) — self-contained, no .tscn to keep in sync.
class_name MeridianBagsWindow
extends PanelContainer

const Money := preload("res://hud/money.gd")

const WIN_W := 300.0

var _bus: MeridianEventBus
var _money_label: Label
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.currency_changed.connect(_on_currency_changed)
	_render_money()


func set_frame_visible(v: bool) -> void:
	visible = v


# Toggle visibility (bound to a HUD key by the world scene). Refreshes on show.
func toggle() -> void:
	if not _built:
		_build()
	set_frame_visible(not visible)
	if visible:
		_render_money()


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

	# The honest gap notice — the item grid needs a wire contract that does not exist yet.
	var gap := Label.new()
	gap.text = "Item contents are not sent by the server yet (no INVENTORY_LIST on the wire). Money above is server-authoritative."
	gap.add_theme_color_override("font_color", Color(0.7, 0.72, 0.78))
	gap.add_theme_font_size_override("font_size", 11)
	gap.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	gap.custom_minimum_size = Vector2(WIN_W - 20.0, 0.0)
	root.add_child(gap)

	var close_row := HBoxContainer.new()
	close_row.alignment = BoxContainer.ALIGNMENT_END
	var close := Button.new()
	close.text = "Close  [B]"
	close.pressed.connect(func(): set_frame_visible(false))
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true
	_render_money()


func _on_currency_changed(_copper: int, _known: bool) -> void:
	_render_money()


func _render_money() -> void:
	if _money_label == null:
		return
	if _bus != null and _bus.currency_known():
		_money_label.text = "Money: %s" % Money.format_copper(_bus.copper())
	else:
		_money_label.text = "Money: (unknown until a transaction)"
