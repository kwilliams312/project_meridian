# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — character-select CONTROLLER + roster view (issue #110 origin,
# redesigned in #639).
#
# The third scene in the boot flow (Boot/Login → CharSelect → World, Client SAD §"GDScript
# interaction path"). After a successful login it owns the live net session, the cached
# roster, and view switching between two focused views (#639):
#
#   * ROSTER VIEW (this scene's %RosterView): the character list, the SELECTED character's
#     paperdoll, and three actions — Delete, Create New Character, Enter Realm. ("Enter
#     Realm" is the button LABEL; the enter-world net intent is unchanged — #639.)
#   * CREATION VIEW (scenes/charselect/character_create.tscn): a self-contained full-screen
#     view this controller reveals on "Create New Character" and hides on create/cancel. It
#     never touches the net layer — it emits character_confirmed / creation_cancelled back
#     here, and THIS controller owns all networking (build_char_create_request_frame, etc.).
#
# This split replaced the old single-screen layout that crammed the roster list AND the
# full create form side by side (#639). The net flow (CharList / CharCreate / CharDelete /
# EnterWorld), the appearance data contract, and the #629 create-result status mapping are
# all preserved.
#
# CHARACTER LIST SOURCE: online = the server roster over the authenticated session
# (CharList); offline (warm boot / tests) = a LOCAL in-memory stub (CharacterStore). See
# character_store.gd for the rationale and the migration path to the real session CRUD.

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

# The reusable paperdoll widget (#639) — builds a SubViewport preview of a character.
# The roster view uses it (front-facing) for the selected character; the creation view
# uses the SAME widget large + drag-to-rotate. Preloaded by path (never the bare class
# name): the headless --script verify has no global class cache — preload is immune.
const PaperdollScript := preload("res://scenes/charselect/character_paperdoll.gd")

# The creation view scene (#639), instanced on demand and revealed full-rect over the
# roster. Preloaded by path for the same reason.
const CharacterCreateScene := preload("res://scenes/charselect/character_create.tscn")

## Render size of the roster paperdoll (the selected character, front-facing). Matches
## the #630 sizing; a minimum, not a trap — under `canvas_items` stretch it scales.
const ROSTER_PAPERDOLL_SIZE := Vector2i(260, 290)

@onready var _roster_view: Control = %RosterView
@onready var _account_label: Label = %AccountLabel
@onready var _char_list: ItemList = %CharList
@onready var _delete_button: Button = %DeleteButton
@onready var _create_new_button: Button = %CreateNewButton
@onready var _enter_button: Button = %EnterWorldButton
@onready var _status: Label = %StatusLabel
@onready var _preview_holder: Control = %PreviewHolder

var _store: CharacterStore
var _account: String = ""
var _session: Dictionary = {}
var _pending_status: String = ""            # set by configure(), shown once in _ready
var _roster_paperdoll: SubViewportContainer = null   # roster view's selected-character preview
var _create_view: Control = null            # the creation view (instanced on first use, #639)

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
	_build_roster_paperdoll()
	_account_label.text = "Account: %s" % (_account if not _account.is_empty() else "(local)")

	_delete_button.pressed.connect(_on_delete_pressed)
	_create_new_button.pressed.connect(_on_create_new_pressed)
	_enter_button.pressed.connect(_on_enter_pressed)
	_char_list.item_selected.connect(_on_char_selected)

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


# --- Roster paperdoll (the selected character's preview) ----------------------

# Build the roster view's paperdoll into %PreviewHolder: the shared widget (#639),
# front-facing (rotation optional on the roster — spec). The selected character's
# persisted race + appearance drive it via _refresh_selected_preview().
# The widget anchors itself full-rect inside the (plain Control) holder (#643), so it
# fills the "Selected character" panel and grows with it — custom_minimum_size is only a
# floor. (No size flags here: %PreviewHolder is a plain Control, which lays children out
# by anchors, not flags.)
func _build_roster_paperdoll() -> void:
	_roster_paperdoll = PaperdollScript.new()
	_roster_paperdoll.name = "Paperdoll"
	_roster_paperdoll.custom_minimum_size = Vector2(ROSTER_PAPERDOLL_SIZE)
	_preview_holder.add_child(_roster_paperdoll)


# Rebuild the character list from the current source. Each row shows "Name — Class (Race)"
# and stores its character id as item metadata so delete/enter read the id, not the row
# index. Re-selects the previously-selected id when it survives the refresh.
# The current roster rows — the server roster cache when online (from CharList), or the
# local store when offline. Each row is { id, name, race, class }.
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
	_refresh_selected_preview()


func _on_char_selected(_index: int) -> void:
	_update_action_buttons()
	_refresh_selected_preview()


# Re-assemble the roster paperdoll from the SELECTED character's persisted appearance +
# race (②/T4, #541 — appearance rides the char-list wire, contract ① T5). Falls back to
# the default record for a row with no appearance (old server / offline seed). No-op when
# nothing is selected (empty roster) — the preview just stays blank.
func _refresh_selected_preview() -> void:
	var row := _character_by_id(_selected_char_id())
	if not row.is_empty():
		_refresh_preview(int(row.get("race", 0)),
			row.get("appearance", MeridianAppearance.default_appearance()), [])


# Mount an AssembledCharacter for (race, appearance) into the roster paperdoll, replacing
# whatever was there (delegates to the shared widget — the capsule fallback lives there).
# Kept as a controller method so the headless verify can drive the roster preview directly.
func _refresh_preview(race: int, appearance: Dictionary, equipment: Array) -> void:
	if _roster_paperdoll != null:
		_roster_paperdoll.set_appearance(race, appearance, equipment)


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


# --- View switching (roster ⇄ creation, #639) ---------------------------------

# "Create New Character" → reveal the creation view (full-rect over the roster). We do
# NOT auto-open it on an empty roster (entry stays explicit — spec §Roster view layout).
func _on_create_new_pressed() -> void:
	_show_create_view()


# Instance the creation view once (lazy) and wire its signals. THIS controller owns all
# networking — the view only hands back the player's chosen values.
func _ensure_create_view() -> Control:
	if _create_view == null:
		_create_view = CharacterCreateScene.instantiate()
		_create_view.character_confirmed.connect(_on_create_confirmed)
		_create_view.creation_cancelled.connect(_on_create_cancelled)
		add_child(_create_view)
		_create_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	return _create_view


func _show_create_view() -> void:
	var view := _ensure_create_view()
	view.reset()
	view.visible = true
	_roster_view.visible = false


func _show_roster_view() -> void:
	if _create_view != null:
		_create_view.visible = false
	_roster_view.visible = true


# The creation view confirmed a character. Validate locally (defense in depth — the view
# already guarded the name), then drive the EXISTING create flow. On the OK result we
# return to the roster and select the new character; a rejection surfaces on the creation
# view (it stays open so the player can fix + retry — spec §Data flow step 5).
func _on_create_confirmed(char_name: String, race: int, char_class: int, appearance: Dictionary) -> void:
	var name := char_name.strip_edges()
	# Local validation mirrors the server's (name non-empty ≤ 32; race/class from the
	# frozen roster). Surface failures on the creation view so the player stays there.
	if name.is_empty():
		_notify_create_error("Enter a name first.")
		return
	if name.length() > 32:
		_notify_create_error("A name can be at most 32 characters.")
		return
	if not MeridianRoster.is_valid_race(race):
		_notify_create_error("Cannot create: invalid race.")
		return
	if not MeridianRoster.is_valid_class(char_class):
		_notify_create_error("Cannot create: invalid class.")
		return

	if _online:
		if not _handshaked:
			_notify_create_error("Still connecting to the realm…")
			return
		var frame: PackedByteArray = _net.build_char_create_request_frame(
			name, race, char_class,
			int(appearance["hair"]), int(appearance["face"]), int(appearance["skin"]))
		if _net.send_bulk(frame):
			_set_status("Creating %s…" % name)
		else:
			_notify_create_error("Could not send the create request.")
		return

	# Offline: create in the local store immediately; the result returns to the roster.
	var result := _store.create(name, race, char_class, appearance)
	if not result.get("ok", false):
		_notify_create_error("Cannot create: %s" % String(result.get("detail", "unknown error")))
		return
	var row: Dictionary = result["row"]
	_refresh_list()
	_select_char_id(int(row["id"]))
	_set_status("Created %s the %s." % [String(row["name"]), MeridianRoster.class_name_for(int(row["class"]))])
	_show_roster_view()


func _on_create_cancelled() -> void:
	# Cancel/Back → return to the roster with NO net traffic (spec §Data flow step 6).
	_show_roster_view()


# Surface a create failure on the creation view (so the player stays there to retry) AND
# mirror it on the roster status line (the single source the #629 regression lock reads).
func _notify_create_error(message: String) -> void:
	_set_status(message)
	if _create_view != null and _create_view.visible:
		_create_view.show_error(message)


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
	_refresh_selected_preview()


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


# Create result: OK re-lists (server is the source of truth) and returns to the roster;
# a typed rejection surfaces on the creation view so the player can fix + retry.
func _on_net_char_create_result(status: int, character_id: int) -> void:
	# world.fbs CharCreateStatus: 0 OK, 1 DUPLICATE_NAME, 2 INVALID_RACE,
	# 3 INVALID_CLASS, 4 INVALID_NAME, 5 INTERNAL, 6 LIMIT_REACHED.
	if status == 0:
		_pending_select_id = character_id  # select the new character once it lists
		_net.send_bulk(_net.build_char_list_request_frame())
		_set_status("Character created.")
		_show_roster_view()  # OK → back to the roster (the new char auto-selects on re-list)
		return
	# #629: the status→message map MUST mirror schema/net/world.fbs CharCreateStatus 1:1.
	# The raw wire status reaches this handler verbatim, so a scrambled map silently
	# mislabels rejections (the reported bug: LIMIT_REACHED falling through to the generic
	# "server error"). Keep exact text; the #629 regression lock asserts it (char_select_verify.gd).
	var reasons := {
		1: "that name is taken", 2: "invalid race", 3: "invalid class",
		4: "invalid name", 5: "server error",
		6: "you already have the maximum number of characters",
	}
	var why: String = reasons.get(status, "server error")
	# Rejection → stay on the creation view with the honest message (spec §Data flow step 5).
	_notify_create_error("Cannot create: %s." % why)


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
