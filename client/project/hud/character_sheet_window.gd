# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the CHARACTER SHEET window (#870, epic #866). The in-world paperdoll:
# your character wearing its gear, the full set of equipment slots (occupied vs empty), and
# click-to-equip / click-to-unequip against the server-authoritative equipment state.
#
# PURE UI WIRING. The equip/unequip backend (#802) already shipped: worldd validates and
# persists every change, re-pushes an INVENTORY_SNAPSHOT (whose `equipment` array is the
# paperdoll projection), and pushes EQUIPMENT_VISUAL_UPDATE so the WORLD re-renders the gear.
# This window therefore builds NOTHING new below the bus — it renders
# MeridianEventBus.equipment_items() and issues request_equip / request_unequip intents.
#
# PURE VIEW / MVVM (the #431/#433 contract): owns NO server state, never touches the net
# thread, never predicts. It renders exactly what the last snapshot carried and re-renders
# when `equipment_changed` fires; a typed `equipment_change_result` only ever produces
# user-visible FEEDBACK TEXT (the following snapshot is still the sole state source — the
# same discipline bags_window.gd follows).
#
# The paperdoll preview REUSES scenes/charselect/character_paperdoll.gd (ratified — do not
# fork): the same widget char-select mounts, driven by (race, appearance, equipment). The
# body comes from the bus's char-select seed (seed_local_appearance); the GEAR it wears is
# the authoritative equipment projection, remapped from the wire's `item_template_id` to the
# assembler's `item_template` key.
#
# Built in code (like bags_window.gd / gossip_window.gd) — self-contained, no .tscn to keep
# in sync. Toggled by [C] from the world scene.
class_name MeridianCharacterSheetWindow
extends PanelContainer

# Preloaded BY PATH, never by bare class name: the headless --script verify has no autoloads
# and a freshly-added class_name is invisible to a stale global class cache (the same reason
# character_paperdoll.gd itself preloads AssembledCharacter by path).
const PaperdollScript := preload("res://scenes/charselect/character_paperdoll.gd")

const WIN_W := 460.0
const PAPERDOLL_SIZE := Vector2(180.0, 240.0)

# The paperdoll POSITIONS, mirroring server/items/src/inventory.h `EquipSlot` — contiguous
# from 0, which is exactly what the wire's `slot` carries. Order here is display order (the
# #866 data contract's head…feet, then jewellery, then weapons); the ids are the contract.
const SLOT_HEAD := 0
const SLOT_SHOULDERS := 1
const SLOT_BACK := 2
const SLOT_CHEST := 3
const SLOT_WRIST := 4
const SLOT_HANDS := 5
const SLOT_WAIST := 6
const SLOT_LEGS := 7
const SLOT_FEET := 8
const SLOT_NECK := 9
const SLOT_FINGER := 10
const SLOT_TRINKET := 11
const SLOT_MAIN_HAND := 12
const SLOT_OFF_HAND := 13
const SLOT_RANGED := 14

# Display order + label for every slot. An unknown slot id on the wire still renders (as
# "Slot N") rather than vanishing — the server stays the authority on what exists.
const _SLOT_ROWS := [
	[SLOT_HEAD, "Head"],
	[SLOT_SHOULDERS, "Shoulders"],
	[SLOT_BACK, "Back"],
	[SLOT_CHEST, "Chest"],
	[SLOT_WRIST, "Wrist"],
	[SLOT_HANDS, "Hands"],
	[SLOT_WAIST, "Waist"],
	[SLOT_LEGS, "Legs"],
	[SLOT_FEET, "Feet"],
	[SLOT_NECK, "Neck"],
	[SLOT_FINGER, "Finger"],
	[SLOT_TRINKET, "Trinket"],
	[SLOT_MAIN_HAND, "Main Hand"],
	[SLOT_OFF_HAND, "Off Hand / Shield"],
	[SLOT_RANGED, "Ranged"],
]

# Rarity tier (item.schema.yaml) → colour. The SAME palette the bags + loot windows use, so
# a player reads item value identically everywhere. 0=poor … 5=legendary.
const _RARITY_COLORS := {
	0: Color(0.62, 0.62, 0.62),   # poor (grey)
	1: Color(1.0, 1.0, 1.0),      # common (white)
	2: Color(0.12, 1.0, 0.0),     # uncommon (green)
	3: Color(0.0, 0.44, 0.87),    # rare (blue)
	4: Color(0.64, 0.21, 0.93),   # epic (purple)
	5: Color(1.0, 0.5, 0.0),      # legendary (orange)
}

const _EMPTY_COLOR := Color(0.55, 0.56, 0.62)

# Typed EQUIPMENT_CHANGE_RESULT status → player-facing feedback. Mirrors the bags window's
# table (one wire contract, one vocabulary) — the class/level/slot rejects the story calls
# out are 5/6/7.
const _RESULT_REASONS := {
	0: "Equipment updated.",
	1: "Enter the world before changing equipment.",
	2: "That slot is invalid.",
	3: "That slot is empty.",
	4: "That item cannot be equipped.",
	5: "Your level is too low for that item.",
	6: "Your class is not proficient with that item.",
	7: "That item's category does not match its equipment slot.",
	8: "That item's equipment type is unknown.",
	9: "Resolve the two-hand/off-hand conflict first.",
	10: "Your backpack is full.",
	11: "Your character class is unknown.",
	12: "The server could not update equipment. Try again.",
}

var _bus: MeridianEventBus
var _paperdoll: SubViewportContainer
var _slot_list: VBoxContainer
var _backpack_list: VBoxContainer
var _status_label: Label
var _pending := false
var _built := false
# The worn set the paperdoll currently displays, so a re-render can skip the (costly)
# re-assemble when nothing it shows actually changed. An INVENTORY_SNAPSHOT is re-pushed
# after EVERY inventory change (loot, vendor buy/sell, quest reward), and the vast majority
# of those do not touch equipment — rebuilding the 3D body on each would blow the HUD's
# ≤2 ms event budget (#431) for no visible difference. `[]` = nothing mounted yet.
var _rendered_equipment: Array = []
var _paperdoll_mounted := false


func _ready() -> void:
	_build()
	set_frame_visible(false)


func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.equipment_changed.connect(_on_equipment_changed)
	_bus.equipment_change_result.connect(_on_equipment_change_result)
	_bus.inventory_changed.connect(_on_inventory_changed)
	_bus.local_appearance_changed.connect(_on_local_appearance_changed)
	_render_all()


func set_frame_visible(v: bool) -> void:
	visible = v


# Toggle visibility (bound to [C] by the world scene). Refreshes on show so a sheet opened
# after a snapshot lands is never stale.
func toggle() -> void:
	if not _built:
		_build()
	set_frame_visible(not visible)
	if visible:
		_render_all()


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(WIN_W, 0.0)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 4)
	add_child(root)

	var title := Label.new()
	title.text = "Character"
	title.add_theme_color_override("font_color", Color(0.85, 0.8, 0.55))
	title.add_theme_color_override("font_outline_color", Color.BLACK)
	title.add_theme_constant_override("outline_size", 4)
	title.add_theme_font_size_override("font_size", 16)
	root.add_child(title)

	root.add_child(HSeparator.new())

	# Top half: the paperdoll preview beside the equipment slot column.
	var top := HBoxContainer.new()
	top.add_theme_constant_override("separation", 8)
	root.add_child(top)

	# The paperdoll goes into a plain Control holder — the widget anchors itself to the
	# holder's full rect (its _ready presets PRESET_FULL_RECT), exactly as char-select
	# mounts it into %PreviewHolder.
	var holder := Control.new()
	holder.name = "PaperdollHolder"
	holder.custom_minimum_size = PAPERDOLL_SIZE
	top.add_child(holder)
	_paperdoll = PaperdollScript.new()
	_paperdoll.name = "Paperdoll"
	_paperdoll.custom_minimum_size = PAPERDOLL_SIZE
	# Let the player spin the figure to inspect their gear from any angle (the creation
	# view already opts into the same drag).
	_paperdoll.draggable = true
	holder.add_child(_paperdoll)

	var slots_col := VBoxContainer.new()
	slots_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slots_col.add_theme_constant_override("separation", 2)
	top.add_child(slots_col)

	var slots_title := Label.new()
	slots_title.text = "Equipment"
	slots_title.add_theme_font_size_override("font_size", 13)
	slots_col.add_child(slots_title)

	_slot_list = VBoxContainer.new()
	_slot_list.add_theme_constant_override("separation", 2)
	slots_col.add_child(_slot_list)

	root.add_child(HSeparator.new())

	var backpack_title := Label.new()
	backpack_title.text = "Backpack"
	backpack_title.add_theme_font_size_override("font_size", 13)
	root.add_child(backpack_title)

	# Click-to-equip source rows. Drag-and-drop is a later story; the INTENT is identical
	# (request_equip on a backpack slot), so this button path is the whole contract.
	_backpack_list = VBoxContainer.new()
	_backpack_list.add_theme_constant_override("separation", 2)
	root.add_child(_backpack_list)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.add_theme_color_override("font_color", Color(0.78, 0.8, 0.86))
	_status_label.add_theme_font_size_override("font_size", 11)
	root.add_child(_status_label)

	var close_row := HBoxContainer.new()
	close_row.alignment = BoxContainer.ALIGNMENT_END
	var close := Button.new()
	close.text = "Close  [C]"
	close.pressed.connect(func(): set_frame_visible(false))
	close_row.add_child(close)
	root.add_child(close_row)

	_built = true
	_render_all()


# --- Bus handlers -------------------------------------------------------------

# The authoritative paperdoll projection changed (a fresh INVENTORY_SNAPSHOT). This is the
# ONLY thing that moves displayed equipment — never a local prediction.
func _on_equipment_changed(_equipment: Array) -> void:
	_pending = false
	_render_all()


# A snapshot's backpack half — re-render the equip-from rows.
func _on_inventory_changed(_money: int, _items: Array, _backpack_slots: int) -> void:
	_pending = false
	_render_backpack()


# Typed reject/accept → user-visible feedback ONLY. On a reject nothing moved server-side,
# so we clear `pending` and re-render from the state we already hold.
func _on_equipment_change_result(result: Dictionary) -> void:
	var status := int(result.get("status", -1))
	if _status_label != null:
		_status_label.text = _result_text(status)
	if status != 0:
		_pending = false
		_render_all()


# The BODY itself changed (the char-select seed landed) — the equipment-unchanged skip in
# _render_paperdoll() must not swallow this, so invalidate the mounted body first.
func _on_local_appearance_changed(_race: int, _sex: int, _appearance: Dictionary) -> void:
	_paperdoll_mounted = false
	_render_paperdoll()


func _result_text(status: int) -> String:
	return String(_RESULT_REASONS.get(status, "Equipment update was rejected."))


# --- Rendering ---------------------------------------------------------------

func _render_all() -> void:
	_render_paperdoll()
	_render_slots()
	_render_backpack()


# Mount the local body wearing the AUTHORITATIVE equipment set. The wire's paperdoll rows
# carry `item_template_id`; the assembler wants `item_template` + `dyes` — remap here rather
# than teaching either side about the other's key (contract ①: per-instance dyes are not on
# the equipment projection yet, so gear renders undyed).
func _render_paperdoll() -> void:
	if _paperdoll == null or _bus == null:
		return
	if not _bus.local_appearance_known():
		return
	var equipment := _paperdoll_equipment()
	# Skip the re-assemble when the worn set is unchanged AND a body is already mounted.
	if _paperdoll_mounted and equipment == _rendered_equipment:
		return
	_paperdoll.set_appearance(_bus.local_race(), _bus.local_appearance(),
		equipment, _bus.local_sex())
	_rendered_equipment = equipment
	_paperdoll_mounted = true


func _paperdoll_equipment() -> Array:
	var out: Array = []
	if _bus == null:
		return out
	for entry in _bus.equipment_items():
		var it := entry as Dictionary
		out.append({
			"slot": int(it.get("slot", 0)),
			"item_template": int(it.get("item_template_id", 0)),
			"dyes": [],
		})
	return out


# Every paperdoll position, occupied or empty. An occupied row shows the item (quality
# colour) + Unequip; an empty row is greyed. Slot ids the client does not know a name for
# still render, keyed off the wire.
func _render_slots() -> void:
	if _slot_list == null:
		return
	for c in _slot_list.get_children():
		c.queue_free()

	if _bus == null or not _bus.inventory_known():
		var waiting := Label.new()
		waiting.text = "Waiting for equipment snapshot…"
		waiting.add_theme_color_override("font_color", _EMPTY_COLOR)
		waiting.add_theme_font_size_override("font_size", 11)
		_slot_list.add_child(waiting)
		return

	var occupied := _equipment_by_slot()
	for row in _SLOT_ROWS:
		_add_slot_row(int(row[0]), String(row[1]), occupied)
	# Anything the server equipped into a slot this client has no name for (a newer server)
	# is still shown — never silently dropped.
	for slot in occupied.keys():
		if not _is_known_slot(int(slot)):
			_add_slot_row(int(slot), "Slot %d" % int(slot), occupied)


func _is_known_slot(slot: int) -> bool:
	for row in _SLOT_ROWS:
		if int(row[0]) == slot:
			return true
	return false


func _equipment_by_slot() -> Dictionary:
	var by_slot := {}
	if _bus == null:
		return by_slot
	for entry in _bus.equipment_items():
		var it := entry as Dictionary
		by_slot[int(it.get("slot", 0))] = it
	return by_slot


func _add_slot_row(slot: int, label_text: String, occupied: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)

	var name_label := Label.new()
	name_label.text = label_text
	name_label.custom_minimum_size = Vector2(110.0, 0.0)
	name_label.add_theme_font_size_override("font_size", 11)
	name_label.add_theme_color_override("font_color", Color(0.72, 0.74, 0.8))
	row.add_child(name_label)

	var item_label := Label.new()
	item_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	item_label.add_theme_color_override("font_outline_color", Color.BLACK)
	item_label.add_theme_constant_override("outline_size", 3)
	item_label.add_theme_font_size_override("font_size", 11)
	row.add_child(item_label)

	if occupied.has(slot):
		var it := occupied[slot] as Dictionary
		var tmpl := int(it.get("item_template_id", 0))
		var quality := int(it.get("quality", 1))
		var bound := int(it.get("binding", 0)) != 0
		var text := "Item #%d" % tmpl
		if bound:
			text += "  (bound)"
		item_label.text = text
		item_label.add_theme_color_override("font_color",
			_RARITY_COLORS.get(quality, _RARITY_COLORS[1]))

		var unequip := Button.new()
		unequip.text = "Unequip"
		unequip.disabled = _pending
		unequip.tooltip_text = "Move Item #%d from %s to your backpack" % [tmpl, label_text]
		unequip.accessibility_name = "Unequip Item %d from %s" % [tmpl, label_text]
		unequip.accessibility_description = \
			"Moves this item to the first available backpack slot."
		unequip.pressed.connect(func(): _request_equipment_change(1, slot))
		row.add_child(unequip)
	else:
		item_label.text = "— empty —"
		item_label.add_theme_color_override("font_color", _EMPTY_COLOR)

	_slot_list.add_child(row)


# The backpack, as the click-to-equip SOURCE. The server decides whether a given item is
# equippable at all (and into which slot) — the client offers the intent for every carried
# item and surfaces the typed reject rather than pre-filtering on guessed rules.
func _render_backpack() -> void:
	if _backpack_list == null:
		return
	for c in _backpack_list.get_children():
		c.queue_free()

	if _bus == null or not _bus.inventory_known():
		var waiting := Label.new()
		waiting.text = "Waiting for inventory snapshot…"
		waiting.add_theme_color_override("font_color", _EMPTY_COLOR)
		waiting.add_theme_font_size_override("font_size", 11)
		_backpack_list.add_child(waiting)
		return

	var items := _bus.inventory_items()
	if items.is_empty():
		var empty := Label.new()
		empty.text = "Your bags are empty."
		empty.add_theme_color_override("font_color", _EMPTY_COLOR)
		empty.add_theme_font_size_override("font_size", 11)
		_backpack_list.add_child(empty)
		return

	for entry in items:
		_add_backpack_row(entry as Dictionary)


func _add_backpack_row(it: Dictionary) -> void:
	var slot := int(it.get("slot", 0))
	var tmpl := int(it.get("item_template_id", 0))
	var count := int(it.get("count", 1))
	var quality := int(it.get("quality", 1))
	var bound := int(it.get("binding", 0)) != 0

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)

	var label := Label.new()
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var text := "Item #%d" % tmpl
	if count > 1:
		text += " x%d" % count
	if bound:
		text += "  (bound)"
	label.text = "[%d] %s" % [slot, text]
	label.add_theme_color_override("font_color",
		_RARITY_COLORS.get(quality, _RARITY_COLORS[1]))
	label.add_theme_color_override("font_outline_color", Color.BLACK)
	label.add_theme_constant_override("outline_size", 3)
	label.add_theme_font_size_override("font_size", 11)
	row.add_child(label)

	var equip := Button.new()
	equip.text = "Equip"
	equip.disabled = _pending
	equip.tooltip_text = "Equip Item #%d from backpack slot %d" % [tmpl, slot]
	equip.accessibility_name = "Equip Item %d from backpack slot %d" % [tmpl, slot]
	equip.accessibility_description = "Moves this item to its server-approved equipment slot."
	equip.pressed.connect(func(): _request_equipment_change(0, slot))
	row.add_child(equip)

	_backpack_list.add_child(row)


# One intent at a time: `pending` disables every button until the server answers (with a
# typed result and/or the authoritative snapshot), so a double-click can't queue two changes.
func _request_equipment_change(action: int, slot: int) -> void:
	if _bus == null or _pending:
		return
	_pending = true
	if _status_label != null:
		_status_label.text = "Updating equipment…"
	_render_slots()
	_render_backpack()
	if action == 0:
		_bus.request_equip(slot)
	else:
		_bus.request_unequip(slot)
