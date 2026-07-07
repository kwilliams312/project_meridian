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
#   * char_select.tscn instantiates, its race/class pickers are populated from the roster,
#     the shared placeholder model is built, the list reflects the store, and Enter World
#     emits the intent for the selected character.
# Exits 0 on success, 1 on any failed assertion.
#
# The interactive playtest is char_select.tscn reached from the login flow; this script is
# the automatable evidence (same convention as scenes/world/camera_verify.gd).

extends SceneTree

var _fails := 0
var _entered_character: Dictionary = {}


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian character-select RUNTIME verify (#110)")

	_verify_roster()
	_verify_store()
	await _verify_scene()

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
	_check("list reflects the create", store.count() == 1)

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


# --- 3. char_select.tscn instantiates and behaves -----------------------------
func _verify_scene() -> void:
	print(" char_select scene:")
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

	var race_opt: OptionButton = scene.find_child("RaceOption", true, false)
	var class_opt: OptionButton = scene.find_child("ClassOption", true, false)
	var char_list: ItemList = scene.find_child("CharList", true, false)
	var preview: Control = scene.find_child("PreviewHolder", true, false)

	_check("race picker populated from roster (4 items)", race_opt != null and race_opt.item_count == 4)
	_check("class picker populated from roster (4 items)", class_opt != null and class_opt.item_count == 4)
	_check("race picker item ids are roster ids",
		race_opt != null and race_opt.get_item_id(0) == 1 and race_opt.get_item_id(3) == 4)
	_check("list shows the two seeded characters", char_list != null and char_list.item_count == 2)
	_check("list label carries name + class",
		char_list != null and char_list.get_item_text(0).begins_with("Kaelith — Vanguard"))

	# The ONE shared placeholder model was built into the preview holder.
	var has_preview := preview != null and preview.get_child_count() > 0 \
		and preview.get_child(0) is SubViewportContainer
	_check("shared placeholder model built into preview", has_preview)

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
