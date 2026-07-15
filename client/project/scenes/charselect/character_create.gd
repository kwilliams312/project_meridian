# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — character CREATION view (issue #639, char-select redesign).
#
# A self-contained full-screen view the char-select CONTROLLER (char_select.gd) reveals
# when the player presses "Create New Character", and hides again on create/cancel. It
# is NOT an OS window and NOT a modal overlay — it replaces the roster view in-scene and
# inherits the #630 `canvas_items` window scaling for free (spec §Decisions).
#
# CONTENTS (spec §Creation view layout):
#   * a LARGE, drag-to-rotate paperdoll (character_paperdoll.gd), live-updating as the
#     race / class / customization pickers change,
#   * race + class + customization (hair / face / skin) as the EXISTING OptionButton
#     dropdowns (spec: keep the dropdowns, laid out larger),
#   * a name LineEdit (max_length 32) centered at the bottom,
#   * Create (confirm) and Cancel/Back actions.
#
# ⛔ THIS VIEW NEVER TOUCHES THE NET LAYER. It emits the player's chosen values back to
# the controller, which owns ALL networking (build_char_create_request_frame, etc.):
#
#   signal character_confirmed(char_name, race, char_class, appearance)   ← Create
#   signal creation_cancelled()                                           ← Cancel/Back
#
# Keeping the net out of here makes the view unit-testable without a session
# (character_create_verify.gd drives it stand-alone). On a server rejection the
# controller calls show_error(...) so the honest message surfaces HERE and the player
# stays on the creation view to fix + retry (spec §Data flow step 5).
#
# The appearance pickers are CATALOG-DRIVEN off MeridianContentDB (#477, spec ② §3) — the
# same logic that used to live in char_select.gd, moved here verbatim: the picker lists
# come from the appearance catalog for the selected race/sex, so the ids offered are
# exactly the stable preset ints the server validates; a race with no catalog disables
# the pickers with a visible "(content missing)" state (spec §6) and the create record
# falls back to the default (the server clamps out-of-range appearance — spec §9).

extends Control

## Emitted when the player confirms creation. The controller validates locally
## (non-empty name ≤ 32; race/class from the frozen roster) and drives the existing
## CHAR_CREATE flow. `appearance` is the {version, hair, face, skin} record.
## `char_class` (not `class` — a GDScript keyword) is the roster class id.
signal character_confirmed(char_name: String, race: int, char_class: int, appearance: Dictionary)

## Emitted on Cancel/Back — the controller returns to the roster with NO net traffic.
signal creation_cancelled()

# MeridianContentDB (#477) by PATH (not the bare autoload name / class name): the
# headless --script verify has no autoloads and a freshly-added class_name is invisible
# to a stale global class cache — preload is immune to both. ContentDb.instance()
# returns the autoload in the running app, else a shared lazily-created instance.
const ContentDb := preload("res://content/content_db.gd")

# The shared paperdoll widget (#639) — preloaded by path for the same reason.
const PaperdollScript := preload("res://scenes/charselect/character_paperdoll.gd")

## Render size of the LARGE creation paperdoll (spec: "large drag-to-rotate paperdoll",
## bigger than the roster preview's 260×290). A minimum, not a trap — under the
## `canvas_items` stretch it grows with the window (#630).
const PAPERDOLL_SIZE := Vector2i(360, 460)

## Sex axis (design §8): male/female over the SHARED body — the picker drives the
## preview's catalog lookup (both sexes share the chibi body for now; differentiation
## via hair/makeup is a later theme addition). 0 = male, 1 = female (content_db._sex_name).
const SEX_OPTIONS: Array = [{"id": 0, "name": "Male"}, {"id": 1, "name": "Female"}]

@onready var _preview_holder: Control = %PreviewHolder
@onready var _race_option: OptionButton = %RaceOption
@onready var _class_option: OptionButton = %ClassOption
@onready var _sex_option: OptionButton = %SexOption
@onready var _hair_option: OptionButton = %HairOption
@onready var _face_option: OptionButton = %FaceOption
@onready var _skin_option: OptionButton = %SkinOption
@onready var _name_edit: LineEdit = %NameEdit
@onready var _create_button: Button = %CreateButton
@onready var _cancel_button: Button = %CancelButton
@onready var _error_label: Label = %ErrorLabel

var _paperdoll: SubViewportContainer = null
var _content_missing: bool = false          # no appearance catalog for the selected race (#477, spec §6)


func _ready() -> void:
	# The large drag-to-rotate paperdoll (#639) — built in code so the .tscn stays a
	# plain Control tree. Expands to fill its panel; min size gives it presence.
	_paperdoll = PaperdollScript.new()
	_paperdoll.name = "Paperdoll"
	_paperdoll.draggable = true
	_paperdoll.custom_minimum_size = Vector2(PAPERDOLL_SIZE)
	_paperdoll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_paperdoll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_preview_holder.add_child(_paperdoll)

	_populate_pickers()

	_create_button.pressed.connect(_on_create_pressed)
	_cancel_button.pressed.connect(_on_cancel_pressed)
	_name_edit.text_submitted.connect(func(_t: String) -> void: _on_create_pressed())
	# Any appearance pick re-assembles the paperdoll so the choice is visible live
	# (②/T4, #541 — the preview is the SAME AssembledCharacter the world builds).
	_hair_option.item_selected.connect(func(_i: int) -> void: _refresh_preview_from_form())
	_face_option.item_selected.connect(func(_i: int) -> void: _refresh_preview_from_form())
	_skin_option.item_selected.connect(func(_i: int) -> void: _refresh_preview_from_form())
	# Changing race re-drives the appearance pickers from that race's catalog (#477),
	# which then re-assembles the preview for the new race.
	_race_option.item_selected.connect(func(_i: int) -> void: _populate_appearance_pickers(_race_option.get_selected_id()))
	# Changing sex re-drives the appearance pickers from the male/female catalog (both
	# sexes share the chibi body for now) and re-assembles the preview for the new sex.
	_sex_option.item_selected.connect(func(_i: int) -> void: _populate_appearance_pickers(_race_option.get_selected_id()))


# Called by the controller each time the view is (re)shown: clear the name field and
# any stale error so the player starts fresh (spec: return-to-create is a clean slate).
func reset() -> void:
	if _name_edit != null:
		_name_edit.clear()
	if _error_label != null:
		_error_label.text = ""


# Surface an honest failure (a local guard OR a server rejection the controller maps via
# the #629 CharCreateStatus→message table) and STAY on the creation view so the player
# can fix + retry (spec §Data flow step 5).
func show_error(message: String) -> void:
	if _error_label != null:
		_error_label.text = message


# --- Pickers (moved verbatim from char_select.gd — the controller no longer owns them) -

# Fill the race + class + sex + appearance pickers. Race/class are PACK-DRIVEN off the
# mounted theme pack's roster (design §8/R3 — the chibi realm shows its 6 colour races ×
# 4 classes), falling back to the compiled MeridianRoster for a pack that ships no roster
# (core). hair/face/skin are CATALOG-DRIVEN off MeridianContentDB (#477, spec ② §3). Each
# item carries its id so the create record reads ids, never list indices.
func _populate_pickers() -> void:
	var races: Array = ContentDb.instance().effective_races()
	var classes: Array = ContentDb.instance().effective_classes()
	_race_option.clear()
	for r in races:
		_race_option.add_item(String(r["name"]), int(r["id"]))
	_class_option.clear()
	for c in classes:
		_class_option.add_item(String(c["name"]), int(c["id"]))
	_sex_option.clear()
	for s in SEX_OPTIONS:
		_sex_option.add_item(String(s["name"]), int(s["id"]))
	# Default to the roster's FIRST race/class (pack order = roster_id; the chibi race
	# picker IS the colour choice, defaulting to the first colour) and male.
	if not races.is_empty():
		_select_option_by_id(_race_option, int(races[0]["id"]))
	if not classes.is_empty():
		_select_option_by_id(_class_option, int(classes[0]["id"]))
	_select_option_by_id(_sex_option, 0)
	_populate_appearance_pickers(_race_option.get_selected_id())


# (Re)fill the hair/face/skin pickers from the appearance catalog for `race_id` at the
# selected sex. A mounted catalog with presets → each preset id becomes a picker item and
# the pickers enable; no catalog OR empty preset lists (a chibi colour race ships EMPTY
# presets for now — appearance customization is a later theme addition) → the pickers
# disable with a "(content missing)" placeholder (spec §6), and _selected_appearance falls
# back to the default record.
func _populate_appearance_pickers(race_id: int) -> void:
	var cat: Dictionary = ContentDb.instance().catalog(race_id, _sex_option.get_selected_id())
	var presets: Dictionary = cat.get("presets", {})
	# Empty preset lists count as content-missing so the record cleanly defaults (a chibi
	# race has a catalog + body_material but no hair/face/skin presets yet).
	_content_missing = cat.is_empty() or presets.get("hair", []).is_empty()
	_fill_preset_picker(_hair_option, presets.get("hair", []), "Hair")
	_fill_preset_picker(_face_option, presets.get("face", []), "Face")
	_fill_preset_picker(_skin_option, presets.get("skin", []), "Skin")
	if not _content_missing:
		# Default to the first preset of each channel (the catalog lists are ordered).
		_select_option_by_id(_hair_option, MeridianAppearance.DEFAULT_HAIR_ID)
		_select_option_by_id(_face_option, MeridianAppearance.DEFAULT_FACE_ID)
		_select_option_by_id(_skin_option, MeridianAppearance.DEFAULT_SKIN_ID)
	_refresh_preview_from_form()


# Fill one preset picker from a catalog preset list ([{id, model}, ...]). Each item's id
# IS the stable preset int. An empty list (no catalog for this race) disables the picker
# with a single "(content missing)" item so the state is visible, not blank.
func _fill_preset_picker(option: OptionButton, presets: Array, channel: String) -> void:
	option.clear()
	if presets.is_empty():
		option.add_item("(content missing)")
		option.disabled = true
		return
	for p in presets:
		option.add_item("%s %d" % [channel, int(p["id"])], int(p["id"]))
	option.disabled = false


# The appearance record the form currently shows: {version, hair, face, skin}. Ids come
# straight off the pickers (each item id IS the preset id). With no catalog for the race
# the record falls back to the default (the server clamps any out-of-range appearance, so
# a create still succeeds — spec §9).
func _selected_appearance() -> Dictionary:
	if _content_missing:
		return MeridianAppearance.default_appearance()
	return {
		"version": MeridianAppearance.VERSION,
		"hair": _hair_option.get_selected_id(),
		"face": _face_option.get_selected_id(),
		"skin": _skin_option.get_selected_id(),
	}


func _select_option_by_id(option: OptionButton, id: int) -> void:
	for i in range(option.item_count):
		if option.get_item_id(i) == id:
			option.select(i)
			return


# Re-assemble the paperdoll from the CURRENT form state (selected race + sex + the
# appearance record the pickers report). Called on every appearance/race/sex pick — the
# live update. For a chibi colour race this assembles the coloured body (body_material dye).
func _refresh_preview_from_form() -> void:
	if _paperdoll != null:
		_paperdoll.set_appearance(_race_option.get_selected_id(), _selected_appearance(), [],
			_sex_option.get_selected_id())


# --- Actions -----------------------------------------------------------------

# Create: validate the name locally (so an empty name surfaces WITHOUT a round trip and
# the player stays here), then hand the chosen values to the controller. The controller
# re-validates (defense in depth) and owns the actual CHAR_CREATE send.
func _on_create_pressed() -> void:
	var char_name := _name_edit.text.strip_edges()
	if char_name.is_empty():
		show_error("Enter a name first.")
		return
	if char_name.length() > 32:
		# Belt-and-braces: max_length already caps input at 32; guard anyway so a
		# programmatic set can't slip an over-long name past to the server.
		show_error("A name can be at most 32 characters.")
		return
	character_confirmed.emit(
		char_name,
		_race_option.get_selected_id(),
		_class_option.get_selected_id(),
		_selected_appearance())


func _on_cancel_pressed() -> void:
	creation_cancelled.emit()
