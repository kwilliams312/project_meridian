# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the character-select stub
# (issue #110, CHR-01 / D-11). NOT a shipped scene: this is a SceneTree script run under
#   godot --headless --script res://scenes/charselect/char_select_verify.gd
# so CI / a dev box can prove, with no render and no server:
#   * the client roster mirror (MeridianRoster) matches the server D-11 ids/names,
#   * the local char-list stub (CharacterStore) enforces the SAME create rules as the
#     server (empty / over-long name, bad race, bad class, duplicate name), lists in
#     creation order, and deletes,
#   * char_select.tscn instantiates as the roster view + controller (#639): the list
#     reflects the store, the selected-character paperdoll builds, the three actions carry
#     the "Enter Realm" relabel, Create New Character switches to the creation view (and
#     Cancel/confirm return), the offline create path lands a character, the #629
#     create-result status→message map still asserts exact text, and Enter World emits the
#     intent for the selected character. (The create FORM itself is now in its own scene,
#     unit-tested by character_create_verify.gd.)
#   * #327 REGRESSION: the roster is repopulated from the authoritative session context on
#     EVERY (re-)login — a fresh scene configured from the same session roster shows the
#     characters again (not an empty list), and a no-roster login shows nothing stale.
# Exits 0 on success, 1 on any failed assertion.
#
# The interactive playtest is char_select.tscn reached from the login flow; this script is
# the automatable evidence (same convention as scenes/world/camera_verify.gd).

extends SceneTree

# MeridianContentDB (#477) by PATH — standalone --script mode has no autoloads and
# a freshly-added class_name may be missing from a stale global class cache.
const ContentDbScript := preload("res://content/content_db.gd")

var _fails := 0
var _entered_character: Dictionary = {}


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian character-select RUNTIME verify (#110)")

	_verify_roster()
	_verify_appearance()
	_verify_store()
	await _verify_scene()
	await _verify_relogin_roster()

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


# --- 1. Client roster mirror matches the server D-11 roster (roster.h) ---------
func _verify_roster() -> void:
	print(" roster mirror (⇄ server/characters/src/roster.h):")
	_check("4 races frozen", MeridianRoster.RACES.size() == 4 and MeridianRoster.RACE_COUNT == 4)
	_check("4 classes frozen", MeridianRoster.CLASSES.size() == 4 and MeridianRoster.CLASS_COUNT == 4)
	# Exact id → name mapping, mirroring roster.h race_name / class_name.
	_check("race ids/names match", (
		MeridianRoster.race_name(1) == "Ardent"
		and MeridianRoster.race_name(2) == "Dolmen"
		and MeridianRoster.race_name(3) == "Sylvane"
		and MeridianRoster.race_name(4) == "Emberkin"))
	_check("class ids/names match", (
		MeridianRoster.class_name_for(1) == "Vanguard"
		and MeridianRoster.class_name_for(2) == "Runcaller"
		and MeridianRoster.class_name_for(3) == "Warden"
		and MeridianRoster.class_name_for(4) == "Mender"))
	# id 0 is reserved invalid; range is [1,4].
	_check("id 0 rejected as unset/invalid",
		not MeridianRoster.is_valid_race(0) and not MeridianRoster.is_valid_class(0))
	_check("out-of-range ids rejected",
		not MeridianRoster.is_valid_race(5) and not MeridianRoster.is_valid_class(5))
	_check("valid range accepted",
		MeridianRoster.is_valid_race(1) and MeridianRoster.is_valid_race(4)
		and MeridianRoster.is_valid_class(1) and MeridianRoster.is_valid_class(4))


# --- 1b. Appearance placeholder set (CHR-01 #435) -----------------------------
# The M1 appearance set (MeridianAppearance) is a documented client-side placeholder
# (no server catalog is exposed to the client yet — see character_appearance.gd). These
# checks assert the preset shape the create form and the wire Appearance record rely on:
# 1-based ids, a working default, and skin-tint lookup.
func _verify_appearance() -> void:
	print(" appearance placeholder set (CHR-01 #435):")
	_check("hair/face/skin presets non-empty",
		not MeridianAppearance.HAIR.is_empty()
		and not MeridianAppearance.FACE.is_empty()
		and not MeridianAppearance.SKIN.is_empty())
	_check("preset ids are 1-based and valid",
		MeridianAppearance.is_valid_hair(1) and MeridianAppearance.is_valid_face(1)
		and MeridianAppearance.is_valid_skin(1))
	_check("id 0 rejected (unset)",
		not MeridianAppearance.is_valid_hair(0) and not MeridianAppearance.is_valid_skin(0))
	var def := MeridianAppearance.default_appearance()
	_check("default appearance is v1 with valid presets",
		int(def["version"]) == MeridianAppearance.VERSION
		and MeridianAppearance.is_valid_hair(int(def["hair"]))
		and MeridianAppearance.is_valid_face(int(def["face"]))
		and MeridianAppearance.is_valid_skin(int(def["skin"])))
	_check("skin_color returns a distinct tint per preset",
		MeridianAppearance.SKIN.size() < 2
		or MeridianAppearance.skin_color(1) != MeridianAppearance.skin_color(2))

	# The create INTENT crosses the real GDExtension bridge (MeridianNetThread) — the
	# same C++ path the online create uses. Proves the appearance-carrying signature
	# (name, race, class, hair, face, skin) binds + builds a wire frame at runtime (#435).
	var net := MeridianNetThread.new()
	var themed_frame: PackedByteArray = net.build_char_create_request_frame(
		"Kaelith", 1, 1, 2, 3, 1)
	var plain_frame: PackedByteArray = net.build_char_create_request_frame(
		"Kaelith", 1, 1, 1, 1, 1)
	_check("bridge builds a CHAR_CREATE frame carrying the appearance set",
		not themed_frame.is_empty() and themed_frame.size() >= plain_frame.size())


# --- 2. Local char store enforces the server's create_character rules ----------
func _verify_store() -> void:
	print(" char store stub (⇄ server create_character validation order):")
	var store := CharacterStore.new()
	_check("fresh account is empty", store.count() == 0 and store.list().is_empty())

	# Happy path — Vanguard Ardent.
	var ok := store.create("Kaelith", 1, 1)
	_check("valid create succeeds", ok.get("ok", false) and int(ok["row"]["id"]) > 0)
	_check("stored row keeps race + class ids",
		int(ok["row"]["race"]) == 1 and int(ok["row"]["class"]) == 1)
	_check("create with no appearance defaults to the v1 record",
		int(ok["row"]["appearance"]["version"]) == MeridianAppearance.VERSION
		and int(ok["row"]["appearance"]["hair"]) == MeridianAppearance.DEFAULT_HAIR_ID)
	_check("list reflects the create", store.count() == 1)

	# Appearance is carried verbatim onto the row when supplied (#435).
	var look := {"version": 1, "hair": 2, "face": 3, "skin": 1}
	var themed := store.create("Sylwen", 3, 2, look)
	_check("create carries the chosen appearance set",
		themed.get("ok", false)
		and int(themed["row"]["appearance"]["hair"]) == 2
		and int(themed["row"]["appearance"]["face"]) == 3
		and int(themed["row"]["appearance"]["skin"]) == 1)

	# 1. empty name.
	_check("empty name rejected (invalid_name)",
		store.create("   ", 1, 1).get("error", "") == "invalid_name")
	# 1b. over-long name (> 32).
	var long_name := "x".repeat(33)
	_check("over-long name rejected (invalid_name)",
		store.create(long_name, 1, 1).get("error", "") == "invalid_name")
	_check("exactly 32 chars accepted",
		store.create("y".repeat(32), 1, 1).get("ok", false))
	# 2. bad race.
	_check("bad race rejected (invalid_race)",
		store.create("Roevil", 0, 1).get("error", "") == "invalid_race")
	# 3. bad class.
	_check("bad class rejected (invalid_class)",
		store.create("Roevil", 1, 9).get("error", "") == "invalid_class")
	# 4. duplicate name, case-insensitive (uq_character_name).
	_check("duplicate name rejected case-insensitively (duplicate_name)",
		store.create("KAELITH", 2, 2).get("error", "") == "duplicate_name")

	# Validation ORDER matches the server: name checked before race before class.
	_check("name checked before race (empty name + bad race → invalid_name)",
		store.create("", 0, 0).get("error", "") == "invalid_name")

	# Delete.
	var first_id := int(ok["row"]["id"])
	var before := store.count()
	_check("delete removes the row", store.delete(first_id) and store.count() == before - 1)
	_check("delete of absent id is a no-op", not store.delete(999999))


# --- 3. char_select roster view + controller (#639) ---------------------------
# The redesign (#639) moved the create FORM out of this scene into character_create.tscn
# (unit-tested by character_create_verify.gd). What remains HERE is the roster view + the
# controller: the character list, the selected-character paperdoll, the three roster
# actions (with the "Enter Realm" relabel), view switching to/from the creation view, the
# offline create path, and the #629 create-result status→message regression lock (the map
# still lives on the controller; re-pointed to also surface on the creation view).
func _verify_scene() -> void:
	print(" char_select roster view + controller (#639):")
	var packed: PackedScene = load("res://scenes/charselect/char_select.tscn")
	_check("char_select.tscn loads", packed != null)
	if packed == null:
		return

	var scene := packed.instantiate()
	# Seed two characters BEFORE the scene enters the tree (the login-handoff path).
	scene.configure("tester@example.com", [
		{"name": "Kaelith", "race": 1, "class": 1},
		{"name": "Sylwen", "race": 3, "class": 2},
	])
	root.add_child(scene)
	await process_frame  # let _ready() run

	var char_list: ItemList = scene.find_child("CharList", true, false)
	var preview: Control = scene.find_child("PreviewHolder", true, false)
	var roster_view: Control = scene.find_child("RosterView", true, false)
	var enter_btn: Button = scene.find_child("EnterWorldButton", true, false)
	var create_btn: Button = scene.find_child("CreateNewButton", true, false)
	var delete_btn: Button = scene.find_child("DeleteButton", true, false)

	# The roster shows the list and the three #639 actions, with "Enter Realm" (the
	# relabel — the enter-world net intent is unchanged, only the button text).
	_check("roster has Enter / Create New / Delete action buttons",
		enter_btn != null and create_btn != null and delete_btn != null)
	_check("Enter button relabeled to 'Enter Realm' (#639)",
		enter_btn != null and enter_btn.text == "Enter Realm")
	_check("Create New Character button present with its label",
		create_btn != null and create_btn.text == "Create New Character")
	_check("list shows the two seeded characters", char_list != null and char_list.item_count == 2)
	_check("list label carries name + class",
		char_list != null and char_list.get_item_text(0).begins_with("Kaelith — Vanguard"))

	# Selection gating (unchanged): no selection → Enter/Delete disabled, Create New always
	# enabled; a selection enables Enter/Delete.
	_check("no selection disables Enter/Delete; Create New stays enabled",
		enter_btn.disabled and delete_btn.disabled and not create_btn.disabled)
	char_list.select(0)
	char_list.item_selected.emit(0)
	_check("selecting a character enables Enter/Delete",
		not enter_btn.disabled and not delete_btn.disabled)

	# The roster paperdoll is built into the holder (a SubViewport render surface, the
	# shared #639 widget). It scales with the window under `canvas_items` stretch (#630).
	var has_preview := preview != null and preview.get_child_count() > 0 \
		and preview.get_child(0) is SubViewportContainer
	_check("roster paperdoll built into the preview holder", has_preview)

	# Roster selection re-assembles from a character's PERSISTED appearance (contract ①
	# T5 wire, driven here directly). A persisted hair preset id 2 → the assembled preview
	# resolves catalog entry 2 (proves the persisted look, not the default, drives it).
	scene._refresh_preview(MeridianRoster.DEFAULT_RACE_ID,
		{"version": 1, "hair": 2, "face": 1, "skin": 1}, [])
	var looked_body: Node = scene.find_child("PreviewBody", true, false)
	_check("roster preview mounts an AssembledCharacter for the default race",
		looked_body != null and looked_body.has_method("applied_preset"))
	_check("roster preview reflects the persisted hair preset (id 2, not the default)",
		looked_body != null and int(looked_body.applied_preset("hair").get("id", 0)) == 2)
	# A race with NO catalog (Sylvane, id 3) degrades the preview to the tinted capsule
	# fallback (spec §6) rather than an empty/assembled body.
	scene._refresh_preview(3, MeridianAppearance.default_appearance(), [])
	var fallback_body: Node = scene.find_child("PreviewBody", true, false)
	_check("no-catalog race degrades the roster preview to a capsule fallback",
		fallback_body is MeshInstance3D and (fallback_body as MeshInstance3D).mesh is CapsuleMesh)

	# --- View switching (#639): roster ⇄ creation, no net traffic offline ----------
	_check("scene starts on the roster view (creation view not yet instanced)",
		roster_view != null and roster_view.visible and scene._create_view == null)
	create_btn.pressed.emit()
	_check("Create New Character reveals the creation view AND hides the roster",
		scene._create_view != null and scene._create_view.visible and not roster_view.visible)
	# Cancel returns to the roster with NO net traffic (offline scene never opened _net).
	scene._create_view.creation_cancelled.emit()
	_check("Cancel returns to the roster with no net traffic",
		roster_view.visible and not scene._create_view.visible and scene._net == null)

	# character_confirmed drives the offline create path: the new character lands, is
	# auto-selected, and we return to the roster.
	create_btn.pressed.emit()
	scene._create_view.character_confirmed.emit(
		"Roevil", 1, 1, MeridianAppearance.default_appearance())
	_check("character_confirmed creates the character (offline) and returns to the roster",
		roster_view.visible and not scene._create_view.visible and char_list.item_count == 3)
	_check("the new character is auto-selected after create",
		String(scene._character_by_id(scene._selected_char_id()).get("name", "")) == "Roevil")

	# #629 REGRESSION LOCK: the create-result status→message map MUST mirror
	# schema/net/world.fbs CharCreateStatus 1:1. The raw wire status reaches this handler
	# verbatim (meridian_net_thread emits msg.char_create.status), so a scrambled map
	# silently mislabels rejections — the reported bug was LIMIT_REACHED (6) falling
	# through to the generic "server error". Drive the error path directly (status != OK
	# never touches _net) and assert the exact text on the status line (the render site the
	# lock reads). If world.fbs renumbers, these must be updated.
	var create_msgs := {
		1: "Cannot create: that name is taken.",                              # DUPLICATE_NAME
		2: "Cannot create: invalid race.",                                    # INVALID_RACE
		3: "Cannot create: invalid class.",                                   # INVALID_CLASS
		4: "Cannot create: invalid name.",                                    # INVALID_NAME
		5: "Cannot create: server error.",                                    # INTERNAL
		6: "Cannot create: you already have the maximum number of characters.",  # LIMIT_REACHED
	}
	for wire_status: int in create_msgs:
		scene._on_net_char_create_result(wire_status, 0)
		_check("create result %d maps to world.fbs message" % wire_status,
			scene._status.text == create_msgs[wire_status])
	# The exact regression: LIMIT_REACHED must be honest, NOT the generic fallback.
	scene._on_net_char_create_result(6, 0)
	_check("LIMIT_REACHED (6) is not the generic 'server error' fallback",
		scene._status.text != "Cannot create: server error.")
	# A truly-unknown code still falls back to the generic message.
	scene._on_net_char_create_result(99, 0)
	_check("unknown create status falls back to 'server error'",
		scene._status.text == "Cannot create: server error.")

	# RE-POINT (#639): a rejection WHILE the creation view is open surfaces the honest
	# message ON that view (so the player stays there to fix + retry, spec §Data flow 5),
	# in addition to the status line the lock above reads.
	create_btn.pressed.emit()  # reopen the creation view (reset() clears any stale error)
	scene._on_net_char_create_result(1, 0)  # DUPLICATE_NAME
	var err_label: Label = scene._create_view.find_child("ErrorLabel", true, false)
	_check("a rejection surfaces on the open creation view (stays open to retry)",
		scene._create_view.visible and err_label != null
		and err_label.text == "Cannot create: that name is taken.")
	scene._show_roster_view()

	# Enter World emits the intent for the SELECTED character. Detach the scene first so
	# the enter guard (is_inside_tree()) skips the real change_scene_to_file — we are
	# asserting the signal contract, not loading the world map (which needs the C++ camera).
	scene.enter_world_requested.connect(func(c: Dictionary) -> void: _entered_character = c)
	char_list.select(0)
	root.remove_child(scene)
	scene._on_enter_pressed()  # underscore is convention only in GDScript
	_check("Enter World emitted for the selected character",
		String(_entered_character.get("name", "")) == "Kaelith"
		and int(_entered_character.get("class", 0)) == 1)

	scene.queue_free()


# --- 4. #327 REGRESSION: the roster is repopulated on a fresh (re-)login -------
# Guards the reported bug "character list is empty on a second login". The roster
# must be re-established from the AUTHORITATIVE session context on every entry, not
# left to stale ephemeral local state. Two fresh char_select instances configured
# from the same session roster (simulating login #1 then login #2) must BOTH show
# the characters — and a fresh instance with no roster must show an EMPTY list (no
# stale carry-over from a prior instance).
func _verify_relogin_roster() -> void:
	print(" re-login roster (#327 regression):")
	var packed: PackedScene = load("res://scenes/charselect/char_select.tscn")
	if packed == null:
		_check("char_select.tscn loads (relogin)", false)
		return

	# The session context the login handoff / world bounce-back threads through: the
	# server's roster for this account (the seam a real CharListResponse fills, #279).
	var session := {
		"roster": [
			{"id": 1, "name": "Kaelith", "race": 1, "class": 1},
			{"id": 2, "name": "Sylwen", "race": 3, "class": 2},
		],
	}

	# Login #1: characters appear.
	var first := packed.instantiate()
	first.configure("tester@example.com", [], session)
	root.add_child(first)
	await process_frame
	var first_list: ItemList = first.find_child("CharList", true, false)
	_check("login #1 lists the account's two characters",
		first_list != null and first_list.item_count == 2)
	root.remove_child(first)
	first.queue_free()

	# Login #2: a BRAND-NEW scene + store, same session roster → characters appear
	# AGAIN (the #327 fix: repopulate from the session, never an empty stale store).
	var second := packed.instantiate()
	second.configure("tester@example.com", [], session)
	root.add_child(second)
	await process_frame
	var second_list: ItemList = second.find_child("CharList", true, false)
	_check("login #2 (fresh scene) still lists the two characters — not empty",
		second_list != null and second_list.item_count == 2)
	_check("login #2 label carries the first character",
		second_list != null and second_list.get_item_text(0).begins_with("Kaelith — Vanguard"))
	root.remove_child(second)
	second.queue_free()

	# A fresh instance with NO roster shows an empty list (no stale carry-over).
	var empty := packed.instantiate()
	empty.configure("tester@example.com", [], {})
	root.add_child(empty)
	await process_frame
	var empty_list: ItemList = empty.find_child("CharList", true, false)
	_check("no-roster login shows an empty list (no stale carry-over)",
		empty_list != null and empty_list.item_count == 0)
	root.remove_child(empty)
	empty.queue_free()
