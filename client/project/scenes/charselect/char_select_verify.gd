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
#     emits the intent for the selected character,
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
	var hair_opt: OptionButton = scene.find_child("HairOption", true, false)
	var face_opt: OptionButton = scene.find_child("FaceOption", true, false)
	var skin_opt: OptionButton = scene.find_child("SkinOption", true, false)
	var char_list: ItemList = scene.find_child("CharList", true, false)
	var preview: Control = scene.find_child("PreviewHolder", true, false)

	_check("race picker populated from roster (4 items)", race_opt != null and race_opt.item_count == 4)
	_check("class picker populated from roster (4 items)", class_opt != null and class_opt.item_count == 4)
	_check("race picker item ids are roster ids",
		race_opt != null and race_opt.get_item_id(0) == 1 and race_opt.get_item_id(3) == 4)

	# Appearance pickers are CATALOG-DRIVEN off MeridianContentDB (#477, spec ② §3):
	# the default race (Ardent) has a mounted catalog, so the pickers reflect its
	# preset lists and the item ids are the stable preset ints the server validates.
	# Path-based access — same reason as char_select.gd: no autoloads in --script
	# mode, and preload is immune to a stale global class cache.
	var cat: Dictionary = ContentDbScript.instance().catalog(MeridianRoster.DEFAULT_RACE_ID, 0)
	_check("ardent catalog is mounted (content staged under res://meridian/core)",
		not cat.is_empty())
	var cat_presets: Dictionary = cat.get("presets", {})
	_check("hair picker populated from the catalog preset list",
		hair_opt != null and not cat_presets.get("hair", []).is_empty()
		and hair_opt.item_count == cat_presets["hair"].size())
	_check("face picker populated from the catalog preset list",
		face_opt != null and face_opt.item_count == cat_presets.get("face", []).size())
	_check("skin picker populated from the catalog preset list",
		skin_opt != null and skin_opt.item_count == cat_presets.get("skin", []).size())
	_check("appearance picker item ids are the catalog preset ids",
		hair_opt != null and hair_opt.get_item_id(0) == int(cat_presets["hair"][0]["id"])
		and not hair_opt.disabled)
	_check("create form reports the selected appearance record",
		scene._selected_appearance()["version"] == MeridianAppearance.VERSION
		and int(scene._selected_appearance()["hair"]) == MeridianAppearance.DEFAULT_HAIR_ID)

	# #590 REGRESSION LOCK: the create UI must expose EVERY catalog preset, not a
	# hardcoded 1-3 range. The S6 catalog upgrade added hair/face preset id 4 (S5
	# hair_4, S1 face_4) — unreachable via the old hardcoded stub, reachable via the
	# catalog-driven pickers (#546). Counts follow the ardent catalog (4 hair, 4 face,
	# 3 skin), and id 4 is present in both channels that carry it. Driven off the mounted
	# catalog, so these cannot silently pass on a stale 1-3 stub.
	_check("hair picker exposes ALL catalog presets incl. id 4 (4 options, id 4 present)",
		hair_opt != null and hair_opt.item_count == 4 and hair_opt.get_item_index(4) != -1)
	_check("face picker exposes ALL catalog presets incl. id 4 (4 options, id 4 present)",
		face_opt != null and face_opt.item_count == 4 and face_opt.get_item_index(4) != -1)
	_check("skin picker exposes ALL catalog presets (3 options)",
		skin_opt != null and skin_opt.item_count == 3)
	# Selecting the formerly-unreachable id 4 flows into the create record verbatim —
	# the stable ints the server validates. Proves id 4 now round-trips through the UI.
	hair_opt.select(hair_opt.get_item_index(4))
	face_opt.select(face_opt.get_item_index(4))
	var look4: Dictionary = scene._selected_appearance()
	_check("selecting hair/face preset id 4 yields a create record with hair=face=4",
		int(look4["hair"]) == 4 and int(look4["face"]) == 4)
	# Restore the default selection so the rest of the scene test runs from a known state.
	hair_opt.select(hair_opt.get_item_index(MeridianAppearance.DEFAULT_HAIR_ID))
	face_opt.select(face_opt.get_item_index(MeridianAppearance.DEFAULT_FACE_ID))

	# Race #2 (Dolmen) D3: selecting Dolmen (id 2) drives the pickers off the NEW
	# dolmen/male catalog (not content-missing, not the ardent one). The catalog
	# resolves to the DOLMEN body, and the pickers enable with the Dolmen preset
	# ids (which reuse the ardent asset ids — same shape, 4 hair / 4 face / 3 skin).
	var dolmen_cat: Dictionary = ContentDbScript.instance().catalog(2, 0)
	_check("dolmen catalog is mounted (Race #2 D3 — catalog(2,0) non-empty)",
		not dolmen_cat.is_empty())
	_check("dolmen catalog resolves to the DOLMEN body (not ardent)",
		String(dolmen_cat.get("body_model", "")) == "core:art.char.dolmen.male.base")
	race_opt.select(race_opt.get_item_index(2))
	race_opt.item_selected.emit(race_opt.get_item_index(2))
	var dolmen_presets: Dictionary = dolmen_cat.get("presets", {})
	_check("selecting Dolmen enables the appearance pickers (has a catalog now)",
		hair_opt != null and not hair_opt.disabled
		and not face_opt.disabled and not skin_opt.disabled)
	_check("Dolmen pickers are populated from the dolmen catalog presets",
		hair_opt != null and hair_opt.item_count == dolmen_presets.get("hair", []).size()
		and face_opt.item_count == dolmen_presets.get("face", []).size()
		and skin_opt.item_count == dolmen_presets.get("skin", []).size())
	# The preview mounts an AssembledCharacter for Dolmen — a real assembled body
	# (with a Dolmen skeleton), NOT the capsule fallback that a no-catalog race gets.
	scene._refresh_preview(2, MeridianAppearance.default_appearance(), [])
	var dolmen_body: Node = scene.find_child("PreviewBody", true, false)
	_check("selecting Dolmen assembles the Dolmen body (not a capsule fallback)",
		dolmen_body != null and dolmen_body.has_method("body_skeleton")
		and dolmen_body.body_skeleton() != null)

	# Content-missing (spec §6): a race with NO catalog (Sylvane, id 3 — only the
	# ardent + dolmen catalogs ship) disables the pickers with a visible "(content
	# missing)" state rather than empty lists, and the create record falls back to
	# the default.
	race_opt.select(race_opt.get_item_index(3))
	race_opt.item_selected.emit(race_opt.get_item_index(3))
	_check("no-catalog race disables the appearance pickers",
		hair_opt != null and hair_opt.disabled and face_opt.disabled and skin_opt.disabled)
	_check("no-catalog race shows a visible 'content missing' item",
		hair_opt != null and hair_opt.item_count == 1
		and hair_opt.get_item_text(0) == "(content missing)")
	_check("content-missing create record falls back to the default appearance",
		int(scene._selected_appearance()["hair"]) == MeridianAppearance.DEFAULT_HAIR_ID)
	# Restore the M1-playable race so the rest of the scene test runs on a real catalog.
	race_opt.select(race_opt.get_item_index(MeridianRoster.DEFAULT_RACE_ID))
	race_opt.item_selected.emit(race_opt.get_item_index(MeridianRoster.DEFAULT_RACE_ID))
	_check("list shows the two seeded characters", char_list != null and char_list.item_count == 2)
	_check("list label carries name + class",
		char_list != null and char_list.get_item_text(0).begins_with("Kaelith — Vanguard"))

	# The preview pane was built into the preview holder (a SubViewport render surface).
	var has_preview := preview != null and preview.get_child_count() > 0 \
		and preview.get_child(0) is SubViewportContainer
	_check("preview pane built into preview holder", has_preview)

	# ②/T4 (#541): the preview is an AssembledCharacter driven by the pickers — the SAME
	# assembly node the world builds — not a bare capsule. For the default (ardent) race
	# it assembles against the mounted catalog, so the preview body has a real skeleton.
	var preview_body: Node = scene.find_child("PreviewBody", true, false)
	_check("preview mounts an AssembledCharacter for the default race",
		preview_body != null and preview_body.has_method("body_skeleton")
		and preview_body.has_method("applied_preset"))

	# Roster selection re-assembles from a character's PERSISTED appearance (contract ①
	# T5 wire, driven here directly). A persisted hair preset id 2 → the assembled preview
	# resolves catalog entry 2 (proves the persisted look, not the default, drives it).
	scene._refresh_preview(MeridianRoster.DEFAULT_RACE_ID,
		{"version": 1, "hair": 2, "face": 1, "skin": 1}, [])
	var looked_body: Node = scene.find_child("PreviewBody", true, false)
	_check("persisted appearance re-assembles the preview",
		looked_body != null and looked_body.has_method("applied_preset"))
	_check("preview reflects the persisted hair preset (id 2, not the default)",
		looked_body != null and int(looked_body.applied_preset("hair").get("id", 0)) == 2)

	# A race with NO catalog (Sylvane, id 3) degrades the preview to the tinted capsule
	# fallback (spec §6) rather than an empty/assembled body.
	scene._refresh_preview(3, MeridianAppearance.default_appearance(), [])
	var fallback_body: Node = scene.find_child("PreviewBody", true, false)
	_check("no-catalog race degrades the preview to a capsule fallback",
		fallback_body is MeshInstance3D and (fallback_body as MeshInstance3D).mesh is CapsuleMesh)
	# Restore an assembled preview for a real race so the scene ends in a valid state.
	scene._refresh_preview(MeridianRoster.DEFAULT_RACE_ID,
		MeridianAppearance.default_appearance(), [])

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
