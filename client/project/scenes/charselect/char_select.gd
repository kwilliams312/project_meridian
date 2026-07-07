# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — minimal character-select screen (issue #110, CHR-01 / D-11).
#
# The third scene in the boot flow (Boot/Login → CharSelect → World, Client SAD §"GDScript
# interaction path"). After a successful login it lists the account's characters (name +
# class), lets the player CREATE one (name entry + race/class pickers drawn from the
# M0-frozen roster) and DELETE one, then ENTER WORLD — which at M0 loads the test map.
#
# SCOPE (D-11): "name entry + class selection over ONE placeholder model" — no appearance
# customization, no per-class models. A single shared capsule stands in for every character
# (built in code below). The server character row still stores a race id AND a class id
# (both NOT NULL), so the create form offers both pickers; both are validated against the
# same roster the server uses (MeridianRoster ⇄ server/characters/src/roster.h).
#
# CHARACTER LIST SOURCE (M0): a LOCAL in-memory stub (CharacterStore) — there is no
# char-management wire message at M0 (world.fbs freezes the opcode set to session/movement/
# entity/clock). See character_store.gd for the full rationale and the migration path to the
# real session-backed CRUD.
#
# Scene shape (char_select.tscn — a plain Control tree; %names resolve regardless of layout):
#   Control (this script)
#     └ Root (VBox)
#         ├ Title / AccountLabel (Label)
#         ├ Body (HBox)
#         │   ├ ListPanel (VBox): CharList (ItemList) + Enter/Delete buttons
#         │   └ CreatePanel (VBox): NameEdit + RaceOption + ClassOption + CreateButton
#         │       + PreviewHolder (Control — the shared placeholder model viewport)
#         └ StatusLabel (Label)

extends Control

## Emitted the instant Enter World is confirmed with a selected character, BEFORE the
## scene transition. `character` is { id, name, race, class }. A future net story listens
## here to send the world handshake / character-bind; at M0 we just load the test map.
signal enter_world_requested(character: Dictionary)

# The Enter World target. #301 makes this the REAL networked world scene (world.tscn):
# it connects to the selected realm's worldd over the net thread and renders remote
# entities moving. When there is NO session context (a warm-boot test / standalone open
# with no login), we fall back to the standalone LOCAL camera sandbox so the scene is
# still openable without a server.
const WORLD_SCENE: String = "res://scenes/world/world.tscn"
const LOCAL_DEMO_SCENE: String = "res://scenes/world/camera_demo.tscn"

@onready var _account_label: Label = %AccountLabel
@onready var _char_list: ItemList = %CharList
@onready var _name_edit: LineEdit = %NameEdit
@onready var _race_option: OptionButton = %RaceOption
@onready var _class_option: OptionButton = %ClassOption
@onready var _create_button: Button = %CreateButton
@onready var _delete_button: Button = %DeleteButton
@onready var _enter_button: Button = %EnterWorldButton
@onready var _status: Label = %StatusLabel
@onready var _preview_holder: Control = %PreviewHolder

var _store: CharacterStore
var _account: String = ""
var _session: Dictionary = {}


# Called by the login handoff BEFORE this scene is added to the tree so the account
# context (and any pre-known characters) are set before _ready populates the UI.
# `seed_rows` is optional and only used by tests / a warm boot without a server.
# `session` is the login session context (#301: grant + session_key + world_hello_frame
# + worldd address:port) carried through so Enter World can open the world session; an
# empty session runs the standalone local demo instead.
func configure(account: String, seed_rows: Array = [], session: Dictionary = {}) -> void:
	_account = account
	_session = session if session != null else {}
	if _store == null:
		_store = CharacterStore.new()
	for row in seed_rows:
		_store.create(String(row.get("name", "")), int(row.get("race", 0)), int(row.get("class", 0)))


func _ready() -> void:
	if _store == null:
		_store = CharacterStore.new()
	_populate_pickers()
	_build_placeholder_preview()
	_account_label.text = "Account: %s" % (_account if not _account.is_empty() else "(local)")

	_create_button.pressed.connect(_on_create_pressed)
	_delete_button.pressed.connect(_on_delete_pressed)
	_enter_button.pressed.connect(_on_enter_pressed)
	_char_list.item_selected.connect(_on_char_selected)
	_name_edit.text_submitted.connect(func(_t: String) -> void: _on_create_pressed())

	_refresh_list()
	_set_status("Select a character to enter the world, or create a new one.")


# Fill the race + class pickers from the M0-frozen roster. Each item carries its roster id
# so create() reads ids, never list indices.
func _populate_pickers() -> void:
	_race_option.clear()
	for r in MeridianRoster.RACES:
		_race_option.add_item(String(r["name"]), int(r["id"]))
	_class_option.clear()
	for c in MeridianRoster.CLASSES:
		_class_option.add_item(String(c["name"]), int(c["id"]))
	# Default to the M1-playable pair (Ardent / Vanguard).
	_select_option_by_id(_race_option, MeridianRoster.DEFAULT_RACE_ID)
	_select_option_by_id(_class_option, MeridianRoster.DEFAULT_CLASS_ID)


func _select_option_by_id(option: OptionButton, id: int) -> void:
	for i in range(option.item_count):
		if option.get_item_id(i) == id:
			option.select(i)
			return


# Rebuild the character list from the store. Each row shows "Name — Class (Race)" and
# stores its character id as item metadata so delete/enter read the id, not the row index.
func _refresh_list() -> void:
	var previously_selected := _selected_char_id()
	_char_list.clear()
	for row in _store.list():
		var label := "%s — %s (%s)" % [
			String(row["name"]),
			MeridianRoster.class_name_for(int(row["class"])),
			MeridianRoster.race_name(int(row["race"])),
		]
		var idx := _char_list.add_item(label)
		_char_list.set_item_metadata(idx, int(row["id"]))
		if int(row["id"]) == previously_selected:
			_char_list.select(idx)
	_update_action_buttons()


func _on_char_selected(_index: int) -> void:
	_update_action_buttons()


# Enter World / Delete are only actionable when a character is selected.
func _update_action_buttons() -> void:
	var has_selection := _selected_char_id() != 0
	_enter_button.disabled = not has_selection
	_delete_button.disabled = not has_selection


func _selected_char_id() -> int:
	var sel := _char_list.get_selected_items()
	if sel.is_empty():
		return 0
	return int(_char_list.get_item_metadata(sel[0]))


func _on_create_pressed() -> void:
	var result := _store.create(
		_name_edit.text, _race_option.get_selected_id(), _class_option.get_selected_id()
	)
	if not result.get("ok", false):
		_set_status("Cannot create: %s" % String(result.get("detail", "unknown error")))
		return
	var row: Dictionary = result["row"]
	_name_edit.clear()
	_refresh_list()
	_select_char_id(int(row["id"]))
	_set_status("Created %s the %s." % [String(row["name"]), MeridianRoster.class_name_for(int(row["class"]))])


func _on_delete_pressed() -> void:
	var id := _selected_char_id()
	if id == 0:
		_set_status("Select a character to delete.")
		return
	if _store.delete(id):
		_refresh_list()
		_set_status("Character deleted.")
	else:
		_set_status("Nothing to delete.")


func _on_enter_pressed() -> void:
	var id := _selected_char_id()
	if id == 0:
		_set_status("Select a character first.")
		return
	var character := _character_by_id(id)
	# Announce the intent, then hand off to the world scene. With a live session context
	# (#301) the REAL networked world scene opens the worldd session bound to this
	# character; without one we fall back to the standalone local camera demo.
	enter_world_requested.emit(character)
	var have_session := not _session.is_empty() and _session.has("world_hello_frame")
	var target := WORLD_SCENE if have_session else LOCAL_DEMO_SCENE
	print("[char_select] enter world as '%s' (race=%d class=%d) → %s%s"
		% [String(character.get("name", "")), int(character.get("race", 0)),
		   int(character.get("class", 0)), target,
		   "" if have_session else " (no session — local demo)"])
	_set_status("Entering world as %s…" % String(character.get("name", "")))
	# Guard: only change scenes when actually inside a running SceneTree (a headless
	# instantiation test builds this node without a current scene).
	if is_inside_tree():
		if have_session:
			_go_to_world(target, character)
		else:
			get_tree().change_scene_to_file(target)


# Instantiate the networked world scene and hand it the session + character BEFORE it
# enters the tree, so world.gd's _ready() connects to worldd with the right context.
func _go_to_world(scene_path: String, character: Dictionary) -> void:
	var packed: PackedScene = load(scene_path)
	if packed == null:
		_set_status("Could not load the world scene.")
		return
	var world := packed.instantiate()
	world.configure(_session, character)
	var tree := get_tree()
	tree.root.add_child(world)
	tree.current_scene = world
	queue_free()


func _character_by_id(id: int) -> Dictionary:
	for row in _store.list():
		if int(row["id"]) == id:
			return row
	return {}


func _select_char_id(id: int) -> void:
	for i in range(_char_list.item_count):
		if int(_char_list.get_item_metadata(i)) == id:
			_char_list.select(i)
			break
	_update_action_buttons()


# Build the ONE shared placeholder character model (D-11: no per-class models). A tiny
# self-contained 3D scene — light + capsule "character" with a nose so facing reads — is
# rendered into a SubViewport inside the create panel. Built in code so the .tscn stays a
# plain Control tree (same convention as scenes/world/camera_demo.gd).
func _build_placeholder_preview() -> void:
	var container := SubViewportContainer.new()
	container.stretch = true
	container.custom_minimum_size = Vector2(180, 220)
	container.size_flags_horizontal = Control.SIZE_SHRINK_CENTER

	var viewport := SubViewport.new()
	viewport.size = Vector2i(180, 220)
	viewport.transparent_bg = true
	container.add_child(viewport)

	var world_root := Node3D.new()
	viewport.add_child(world_root)

	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-45.0), deg_to_rad(35.0), 0.0)
	world_root.add_child(light)

	var body := MeshInstance3D.new()
	body.name = "PlaceholderBody"
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	body.mesh = capsule
	world_root.add_child(body)

	# A small "nose" so the model has an obvious front (facing -Z, Godot forward).
	var nose := MeshInstance3D.new()
	var nm := BoxMesh.new()
	nm.size = Vector3(0.2, 0.2, 0.5)
	nose.mesh = nm
	nose.position = Vector3(0, 0.2, -0.45)
	body.add_child(nose)

	var cam := Camera3D.new()
	cam.position = Vector3(0, 0.2, 3.0)
	cam.current = true
	world_root.add_child(cam)

	_preview_holder.add_child(container)


func _set_status(text: String) -> void:
	if _status != null:
		_status.text = text
