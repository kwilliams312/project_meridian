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

# MeridianContentDB (#477), referenced by PATH (not the bare `ContentDB` autoload
# name, and not the bare class name): the headless verify harness runs standalone
# --script, where autoloads never initialize, and a freshly-added class_name is
# invisible to a stale .godot global class cache — preload is immune to both.
# ContentDb.instance() returns the autoload in the running app (it registers
# itself in _init), else a shared lazily-created instance (verify runs).
const ContentDb := preload("res://content/content_db.gd")

# AssembledCharacter (②/T4, #541): the preview pane renders the SAME assembly node the
# world uses, driven by the create-form pickers (and re-driven from a roster row's
# persisted appearance on selection). Preloaded by path — the headless verify has no
# global class cache. A content miss (no catalog for the race) degrades to the tinted
# capsule fallback, matching the "content missing" picker state (spec §6).
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")

@onready var _account_label: Label = %AccountLabel
@onready var _char_list: ItemList = %CharList
@onready var _name_edit: LineEdit = %NameEdit
@onready var _race_option: OptionButton = %RaceOption
@onready var _class_option: OptionButton = %ClassOption
@onready var _hair_option: OptionButton = %HairOption
@onready var _face_option: OptionButton = %FaceOption
@onready var _skin_option: OptionButton = %SkinOption
@onready var _create_button: Button = %CreateButton
@onready var _delete_button: Button = %DeleteButton
@onready var _enter_button: Button = %EnterWorldButton
@onready var _status: Label = %StatusLabel
@onready var _preview_holder: Control = %PreviewHolder

var _store: CharacterStore
var _account: String = ""
var _session: Dictionary = {}
var _pending_status: String = ""            # set by configure(), shown once in _ready
var _preview_root: Node3D = null            # preview 3D container; holds the AssembledCharacter (②/T4) or capsule fallback
var _content_missing: bool = false          # no appearance catalog for the selected race (#477, spec §6)

# --- Server-authoritative character CRUD over the net thread (#279 / D-35) -----
# When there is a live session (grant + WorldHello frame + worldd address), this
# screen connects to worldd itself and drives the REAL character flow over the
# authenticated session: CharList populates the roster, Create/Delete persist to
# the server, and Enter World sends ENTER_WORLD(character_id) — the world scene is
# opened only on ENTER_WORLD_RESPONSE(OK), reusing THIS live connection (the grant
# is single-use, so world.gd must not reconnect). With NO session (warm boot /
# tests) the screen falls back to the local in-memory CharacterStore + local demo.
var _net: MeridianNetThread = null          # the live world session (online only)
var _online: bool = false                   # a real session is driving this screen
var _roster: Array = []                      # server roster cache (online); rows {id,name,race,class,level}
var _handshaked: bool = false               # HandshakeOk seen (character-select reached)
var _entering: bool = false                 # ENTER_WORLD sent, awaiting the response
var _enter_character: Dictionary = {}       # the character Enter World was pressed for
var _pending_select_id: int = 0             # select this char once the next CharList lands


# Called by the login handoff BEFORE this scene is added to the tree so the account
# context (and any pre-known characters) are set before _ready populates the UI.
# `seed_rows` is optional and only used by tests / a warm boot without a server.
# `session` is the login session context (#301: grant + session_key + world_hello_frame
# + worldd address:port) carried through so Enter World can open the world session; an
# empty session runs the standalone local demo instead.
# `error` is a non-empty message when the world scene bounced the player back here on a
# pre-HandshakeOk connection failure (#301 UX) — surfaced on the status line so the
# player sees WHY they are back, instead of being stranded in an empty world.
#
# ROSTER RE-ESTABLISHMENT (#327). The character roster is re-built from the
# AUTHORITATIVE session context on EVERY entry to this screen, not carried over as
# stale local state. Root cause of #327 ("second login shows an empty list"): the
# roster lived only in this scene's ephemeral CharacterStore, so a fresh login built
# a NEW empty store and the account's characters vanished. Now configure() (a) starts
# from a FRESH store so a reused/re-entered scene never leaks a previous session's
# roster, and (b) repopulates from `session.roster` — the server's list, threaded
# through by the login handoff and the world bounce-back — falling back to `seed_rows`
# only when no session roster is present (tests / warm boot). This is also the single
# seam a real CharListResponse populates once the client char-list transport lands
# (#279): fill `session.roster` from the wire and this screen shows it every time.
func configure(account: String, seed_rows: Array = [], session: Dictionary = {}, error: String = "") -> void:
	_account = account
	_session = session if session != null else {}
	# Online when the login handed us a real world session (grant + WorldHello frame
	# + worldd address). Then this screen connects to worldd itself and the roster is
	# fetched over the wire (CharList) — NOT carried as local state. Offline (no
	# session) keeps the local in-memory store + seed rows for warm boot / tests (#327).
	_online = not _session.is_empty() \
		and (_session.get("world_hello_frame", PackedByteArray()) as PackedByteArray).size() > 0 \
		and not String(_session.get("worldd_host", "")).is_empty()
	# Fresh store on every configure() — never inherit a prior session's rows (#327).
	_store = CharacterStore.new()
	if not _online:
		# Offline: repopulate the local store from the AUTHORITATIVE session roster
		# (the #327 fix — a re-login/bounce-back rebuilds from session, not stale local
		# state), falling back to seed_rows (tests / warm boot). Online skips this: the
		# roster is fetched live via CharList once the session handshakes.
		var roster: Array = _session.get("roster", [])
		var source: Array = roster if not roster.is_empty() else seed_rows
		for row in source:
			_store.create(String(row.get("name", "")), int(row.get("race", 0)), int(row.get("class", 0)))
	if not error.strip_edges().is_empty():
		_pending_status = error


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
	# Any appearance pick re-assembles the preview so the choice is visible before create
	# (②/T4, #541 — the preview is the SAME AssembledCharacter the world builds).
	_hair_option.item_selected.connect(func(_i: int) -> void: _refresh_preview_from_form())
	_face_option.item_selected.connect(func(_i: int) -> void: _refresh_preview_from_form())
	_skin_option.item_selected.connect(func(_i: int) -> void: _refresh_preview_from_form())
	# Changing race re-drives the appearance pickers from that race's catalog (#477),
	# which then re-assembles the preview for the new race.
	_race_option.item_selected.connect(func(_i: int) -> void: _populate_appearance_pickers(_race_option.get_selected_id()))

	_refresh_list()
	# A pending status (e.g. a world connect-failure the player was bounced back with,
	# #301) wins over the default hint so they see why they are back at Character Select.
	if not _pending_status.is_empty():
		_set_status(_pending_status)
		_pending_status = ""
	elif _online:
		_set_status("Connecting to the realm…")
	else:
		_set_status("Select a character to enter the world, or create a new one.")

	# Online: connect to worldd over the net thread and drive the real character flow.
	if _online:
		_connect_online()


# Establish the authenticated world session (WorldHello) so this screen can list/
# create/delete characters over the wire and ENTER_WORLD. The SAME live connection is
# handed to the world scene on a successful enter (the grant is single-use). Signals
# are drained each frame by _process -> _net.pump().
func _connect_online() -> void:
	_net = MeridianNetThread.new()
	_net.handshake_ok.connect(_on_net_handshake_ok)
	_net.char_list.connect(_on_net_char_list)
	_net.char_create_result.connect(_on_net_char_create_result)
	_net.char_delete_result.connect(_on_net_char_delete_result)
	_net.enter_world_result.connect(_on_net_enter_world_result)
	_net.disconnected.connect(_on_net_disconnected)
	_net.connect_failed.connect(_on_net_connect_failed)
	_net.transport_closed.connect(_on_net_transport_closed)

	var host := String(_session.get("worldd_host", ""))
	var port := int(_session.get("worldd_port", 0))
	var frame: PackedByteArray = _session.get("world_hello_frame", PackedByteArray())
	var key: PackedByteArray = _session.get("session_key", PackedByteArray())
	print("[charselect] connecting to worldd %s:%d (world_hello=%d B, key=%d B)"
		% [host, port, frame.size(), key.size()])
	if not _net.connect_to_world(host, port, frame, key):
		_online = false
		_net = null
		print("[charselect] connect_to_world refused (bad WorldHello frame)")
		_set_status("Could not start the world connection.")


# Drain decoded server events each frame (the pre-sim sync point). Only while this
# screen owns the net thread — once handed to the world scene, that scene pumps.
func _process(_delta: float) -> void:
	# Drain server events every frame while this screen owns the net thread — INCLUDING
	# while awaiting the ENTER_WORLD response (do NOT gate on _entering, or the response
	# is never read and Enter World hangs). Draining stops via set_process(false) in
	# _enter_world_scene() once we hand the live session to the world scene.
	if _online and _net != null:
		_net.pump()


# Fill the race + class + appearance pickers. Race/class come from the M0-frozen
# roster; hair/face/skin are CATALOG-DRIVEN off MeridianContentDB (#477, spec ② §3):
# the picker lists come from the appearance catalog for the selected race/sex, so
# the ids offered are exactly the stable preset ints the server validates. When no
# catalog is mounted for a race the pickers disable with a "content missing" state
# (spec §6) instead of showing empty lists. Each item carries its id so create()
# reads ids, never list indices.
func _populate_pickers() -> void:
	_race_option.clear()
	for r in MeridianRoster.RACES:
		_race_option.add_item(String(r["name"]), int(r["id"]))
	_class_option.clear()
	for c in MeridianRoster.CLASSES:
		_class_option.add_item(String(c["name"]), int(c["id"]))
	# Default to the M1-playable pair (Ardent / Vanguard), then drive the appearance
	# pickers from that race's catalog.
	_select_option_by_id(_race_option, MeridianRoster.DEFAULT_RACE_ID)
	_select_option_by_id(_class_option, MeridianRoster.DEFAULT_CLASS_ID)
	_populate_appearance_pickers(_race_option.get_selected_id())


# (Re)fill the hair/face/skin pickers from the appearance catalog for `race_id`
# (M1 sex 0 = male). A mounted catalog → each preset id becomes a picker item and
# the pickers enable; no catalog → the pickers disable with a "(content missing)"
# placeholder (spec §6), and _selected_appearance falls back to the default record.
func _populate_appearance_pickers(race_id: int) -> void:
	var cat: Dictionary = ContentDb.instance().catalog(race_id, 0)
	var presets: Dictionary = cat.get("presets", {})
	_content_missing = cat.is_empty() or not presets.has("hair")
	_fill_preset_picker(_hair_option, presets.get("hair", []), "Hair")
	_fill_preset_picker(_face_option, presets.get("face", []), "Face")
	_fill_preset_picker(_skin_option, presets.get("skin", []), "Skin")
	if not _content_missing:
		# Default to the first preset of each channel (the catalog lists are ordered).
		_select_option_by_id(_hair_option, MeridianAppearance.DEFAULT_HAIR_ID)
		_select_option_by_id(_face_option, MeridianAppearance.DEFAULT_FACE_ID)
		_select_option_by_id(_skin_option, MeridianAppearance.DEFAULT_SKIN_ID)
	_refresh_preview_from_form()


# Fill one preset picker from a catalog preset list ([{id, model}, ...]). Each item's
# id IS the stable preset int. An empty list (no catalog for this race) disables the
# picker with a single "(content missing)" item so the state is visible, not blank.
func _fill_preset_picker(option: OptionButton, presets: Array, channel: String) -> void:
	option.clear()
	if presets.is_empty():
		option.add_item("(content missing)")
		option.disabled = true
		return
	for p in presets:
		option.add_item("%s %d" % [channel, int(p["id"])], int(p["id"]))
	option.disabled = false


# The appearance record the create form currently shows: {version, hair, face, skin}.
# Ids come straight off the pickers (each item id IS the preset id). With no catalog
# for the selected race the record falls back to the default (the server clamps any
# out-of-range appearance, so a create still succeeds — spec §9).
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


# Rebuild the character list from the store. Each row shows "Name — Class (Race)" and
# stores its character id as item metadata so delete/enter read the id, not the row index.
# The current roster rows — the server roster cache when online (from CharList), or
# the local store when offline. Each row is { id, name, race, class }.
func _rows() -> Array:
	return _roster if _online else _store.list()


func _refresh_list() -> void:
	var previously_selected := _selected_char_id()
	_char_list.clear()
	for row in _rows():
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
	# Roster selection re-assembles the preview from THAT character's persisted appearance
	# + race (②/T4, #541 — appearance rides the char-list wire, contract ① T5). Falls back
	# to the default record for a row with no appearance (old server / offline seed).
	var row := _character_by_id(_selected_char_id())
	if not row.is_empty():
		_refresh_preview(int(row.get("race", 0)),
			row.get("appearance", MeridianAppearance.default_appearance()), [])


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
	# Online: send CHAR_CREATE over the session; the roster refreshes on the OK reply
	# (server is the source of truth). Offline: create in the local store immediately.
	if _online:
		if not _handshaked:
			_set_status("Still connecting to the realm…")
			return
		var name := _name_edit.text.strip_edges()
		if name.is_empty():
			_set_status("Enter a name first.")
			return
		var look := _selected_appearance()
		var frame: PackedByteArray = _net.build_char_create_request_frame(
			name, _race_option.get_selected_id(), _class_option.get_selected_id(),
			int(look["hair"]), int(look["face"]), int(look["skin"]))
		if _net.send_bulk(frame):
			_set_status("Creating %s…" % name)
		else:
			_set_status("Could not send the create request.")
		return
	var result := _store.create(
		_name_edit.text, _race_option.get_selected_id(), _class_option.get_selected_id(),
		_selected_appearance()
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
	# Online: send CHAR_DELETE; the roster refreshes on the reply. Offline: local delete.
	if _online:
		var frame: PackedByteArray = _net.build_char_delete_request_frame(id)
		if _net.send_bulk(frame):
			_set_status("Deleting…")
		else:
			_set_status("Could not send the delete request.")
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
	enter_world_requested.emit(character)

	# Online (server-authoritative, D-35): send ENTER_WORLD(character_id) and switch
	# to the world scene ONLY on ENTER_WORLD_RESPONSE(OK) — the server validates the
	# character is owned before spawning. The live connection is carried into the world
	# scene (the grant is single-use, so it must be reused, not reconnected).
	if _online:
		if _entering:
			return  # an ENTER_WORLD is already in flight — ignore repeat clicks
		if not _handshaked:
			_set_status("Still connecting to the realm…")
			return
		_entering = true
		_enter_character = character
		var frame: PackedByteArray = _net.build_enter_world_request_frame(id)
		if _net.send_control(frame):
			_set_status("Entering world as %s…" % String(character.get("name", "")))
		else:
			_entering = false
			_set_status("Could not send the enter request.")
		return

	# Offline fallback: no session → the standalone local camera demo.
	print("[char_select] enter world as '%s' (race=%d class=%d) → %s (no session — local demo)"
		% [String(character.get("name", "")), int(character.get("race", 0)),
		   int(character.get("class", 0)), LOCAL_DEMO_SCENE])
	_set_status("Entering world as %s…" % String(character.get("name", "")))
	if is_inside_tree():
		get_tree().change_scene_to_file(LOCAL_DEMO_SCENE)


func _character_by_id(id: int) -> Dictionary:
	for row in _rows():
		if int(row["id"]) == id:
			return row
	return {}


func _select_char_id(id: int) -> void:
	for i in range(_char_list.item_count):
		if int(_char_list.get_item_metadata(i)) == id:
			_char_list.select(i)
			break
	_update_action_buttons()


# Build the preview pane: a SubViewport (light + camera) with a `_preview_root` the
# character model mounts into. The model is an AssembledCharacter (②/T4, #541) driven by
# the create-form pickers — the SAME assembly node the world scene builds — replacing the
# D-11 shared capsule placeholder. Built in code so the .tscn stays a plain Control tree
# (same convention as scenes/world/camera_demo.gd). _refresh_preview_from_form() mounts
# the first model once the pickers are populated.
func _build_placeholder_preview() -> void:
	# #630: the paperdoll grew (180×220 → 260×290: ~+44% wide, ~+32% tall) to match the
	# larger char-select window, and the SubViewport RENDER size tracks it so the model is
	# drawn at the new resolution (not upscaled-blurry). Height is capped at 290 (not the
	# fuller 330) so the create-form column still fits the 972-tall base window without
	# pushing the Title off-screen — the form is a long single column; see the Theme note in
	# char_select.tscn. Under the project's `canvas_items` stretch the SubViewportContainer
	# — a Control — scales with the window like the rest of the UI, so it also grows on resize.
	var container := SubViewportContainer.new()
	container.stretch = true
	container.custom_minimum_size = Vector2(260, 290)
	container.size_flags_horizontal = Control.SIZE_SHRINK_CENTER

	var viewport := SubViewport.new()
	viewport.size = Vector2i(260, 290)
	viewport.transparent_bg = true
	container.add_child(viewport)

	var world_root := Node3D.new()
	viewport.add_child(world_root)

	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-45.0), deg_to_rad(35.0), 0.0)
	world_root.add_child(light)

	# The model mounts under this container so a re-assemble just clears + rebuilds it.
	_preview_root = Node3D.new()
	_preview_root.name = "PreviewRoot"
	world_root.add_child(_preview_root)

	# Framed for a standing figure (feet at y=0, head ~1.8) rather than the old centered
	# capsule — the assembled body is rooted at the feet.
	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.0, 3.2)
	cam.current = true
	world_root.add_child(cam)

	_preview_holder.add_child(container)
	_refresh_preview_from_form()


# Re-assemble the preview from the CURRENT create-form state (selected race + the
# appearance record the pickers report). Called on every appearance/race pick.
func _refresh_preview_from_form() -> void:
	_refresh_preview(_race_option.get_selected_id(), _selected_appearance(), [])


# Mount an AssembledCharacter for (race, appearance) into the preview, replacing whatever
# was there. On a content miss (no catalog for the race — assemble() false, spec §6) the
# tinted-capsule fallback stands in, matching the "content missing" picker state. No-op
# until the preview root is built (order-independent).
func _refresh_preview(race: int, appearance: Dictionary, equipment: Array) -> void:
	if _preview_root == null:
		return
	for child in _preview_root.get_children():
		child.free()
	var assembled = AssembledCharacterScript.new()
	assembled.name = "PreviewBody"
	var ok: bool = assembled.assemble(race, 0, appearance, equipment)
	if ok:
		_preview_root.add_child(assembled)
		return
	assembled.free()
	_preview_root.add_child(_make_preview_capsule(int(appearance.get("skin", MeridianAppearance.DEFAULT_SKIN_ID))))


# The tinted-capsule fallback (D-11 placeholder), used when no catalog is mounted for the
# selected race (spec §6). Tinted by the chosen skin preset so the pick still reads (#435).
func _make_preview_capsule(skin_id: int) -> MeshInstance3D:
	var body := MeshInstance3D.new()
	body.name = "PreviewBody"
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	body.mesh = capsule
	body.position = Vector3(0, 0.9, 0)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = MeridianAppearance.skin_color(skin_id)
	body.material_override = mat
	# A small "nose" so the model has an obvious front (facing -Z, Godot forward).
	var nose := MeshInstance3D.new()
	var nm := BoxMesh.new()
	nm.size = Vector3(0.2, 0.2, 0.5)
	nose.mesh = nm
	nose.position = Vector3(0, 0.2, -0.45)
	body.add_child(nose)
	return body


# --- Net-thread signal handlers (online only) --------------------------------

# HandshakeOk = authenticated, at character-select. Ask the server for the roster.
func _on_net_handshake_ok() -> void:
	_handshaked = true
	print("[charselect] HandshakeOk — at character-select; requesting CharList")
	_set_status("Loading your characters…")
	_net.send_bulk(_net.build_char_list_request_frame())


# The account's roster arrived — cache it and render. Enter/Delete need a selection;
# an empty roster prompts creation.
# status: world.fbs CharListStatus — 0 OK, 1 INTERNAL. INTERNAL means the server
# could NOT read the roster (DB fault, #479): an empty `characters` here is a LOAD
# FAILURE, not "you own zero characters". Render a retry/error state and DON'T
# clobber the cached roster — otherwise a transient DB fault would make the player's
# existing characters appear to vanish (the exact #479 symptom the client masked).
func _on_net_char_list(characters: Array, status: int = 0) -> void:
	if status != 0:
		print("[charselect] CharList: load FAILED (status=%d)" % status)
		_set_status("Couldn't load your characters — server error. Retry.")
		return
	print("[charselect] CharList: %d character(s)" % characters.size())
	_roster = characters
	_refresh_list()
	if _pending_select_id != 0:
		_select_char_id(_pending_select_id)  # select a just-created character
		_pending_select_id = 0
	if _roster.is_empty():
		_set_status("No characters yet — create one to enter the world.")
	elif _status.text.begins_with("Loading"):
		_set_status("Select a character to enter the world, or create a new one.")


# Create result: OK re-lists (server is the source of truth); else a typed error.
func _on_net_char_create_result(status: int, character_id: int) -> void:
	# world.fbs CharCreateStatus: 0 OK, 1 DUPLICATE_NAME, 2 INVALID_NAME,
	# 3 INVALID_RACE, 4 INVALID_CLASS, 5 LIMIT_REACHED, 6 INTERNAL.
	if status == 0:
		_name_edit.clear()
		_pending_select_id = character_id  # select the new character once it lists
		_net.send_bulk(_net.build_char_list_request_frame())
		_set_status("Character created.")
		return
	var reasons := {
		1: "that name is taken", 2: "invalid name", 3: "invalid race",
		4: "invalid class", 5: "you already have a character",
	}
	var why: String = reasons.get(status, "server error")
	_set_status("Cannot create: %s." % why)


# Delete result: OK re-lists; REFUSED/INTERNAL surfaces a message.
func _on_net_char_delete_result(status: int) -> void:
	if status == 0:
		_net.send_bulk(_net.build_char_list_request_frame())
		_set_status("Character deleted.")
	else:
		_set_status("Could not delete that character.")


# Enter-world result: OK hands the LIVE session to the world scene; else stay here.
func _on_net_enter_world_result(status: int) -> void:
	# world.fbs EnterWorldStatus: 0 OK (spawned), 1 NOT_FOUND, 2 NO_CHARACTER, 3 INTERNAL.
	print("[charselect] ENTER_WORLD result status=%d" % status)
	if status == 0:
		_enter_world_scene()
		return
	_entering = false
	if status == 2:
		_set_status("Create a character before entering the world.")
	elif status == 1:
		_set_status("That character was not found — pick another.")
		_net.send_bulk(_net.build_char_list_request_frame())  # re-sync the roster
	else:
		_set_status("Could not enter the world (server error).")


func _on_net_disconnected(reason: int, message: String) -> void:
	print("[charselect] disconnected reason=%d: %s" % [reason, message])
	_set_status("Disconnected: %s" % message)


func _on_net_connect_failed(detail: String) -> void:
	print("[charselect] connect FAILED: %s" % detail)
	_set_status("Could not reach the realm: %s" % detail)


func _on_net_transport_closed(detail: String) -> void:
	print("[charselect] transport closed: %s" % detail)
	_set_status("The realm connection closed.")


# Hand the LIVE, in-world net thread to the world scene and switch to it. The world
# scene REUSES this connection (the grant is single-use — it must not reconnect).
# char_select stops draining first so the queued AoI EntityEnter frames survive for
# the world scene to consume.
func _enter_world_scene() -> void:
	if not is_inside_tree():
		return
	var live_net := _net
	# Stop receiving on this screen so we don't drain the world scene's inbound frames.
	set_process(false)
	_net = null
	_online = false
	_session["account"] = _account
	_session["net_thread"] = live_net  # world.gd reuses this instead of reconnecting
	var packed: PackedScene = load(WORLD_SCENE)
	if packed == null:
		_set_status("Could not load the world scene.")
		return
	var world := packed.instantiate()
	world.configure(_session, _enter_character)
	var tree := get_tree()
	tree.root.add_child(world)
	tree.current_scene = world
	queue_free()


func _set_status(text: String) -> void:
	if _status != null:
		_status.text = text
