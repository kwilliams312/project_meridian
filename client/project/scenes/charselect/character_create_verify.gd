# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the CHARACTER CREATION view
# (issue #639, char-select redesign). NOT a shipped scene: a SceneTree script run under
#   godot --headless --path client/project --script res://scenes/charselect/character_create_verify.gd
# so CI / a dev box can prove, WITH NO SERVER (the view never touches the net layer):
#   * character_create.tscn instantiates and its race/class pickers are populated from the
#     M0-frozen roster (MeridianRoster) with the roster ids,
#   * the hair/face/skin pickers are CATALOG-DRIVEN off MeridianContentDB (#477) — the
#     #590 lock (every catalog preset exposed incl. id 4), the Dolmen D3 catalog, and the
#     content-missing state (spec §6) all carry over from the old char_select verify,
#   * the two signals fire with the correct payloads — character_confirmed(name, race,
#     class, appearance) on Create, creation_cancelled() on Cancel,
#   * local name validation surfaces WITHOUT emitting (empty name → an error, no confirm),
#   * the large paperdoll live-updates (an AssembledCharacter mounts as PreviewBody).
# Exits 0 on success, 1 on any failed assertion.
#
# The interactive playtest is char_select.tscn → Create New Character; this script is the
# automatable evidence (same convention as scenes/charselect/char_select_verify.gd).

extends SceneTree

# MeridianContentDB (#477) by PATH — standalone --script mode has no autoloads and a
# freshly-added class_name may be missing from a stale global class cache.
const ContentDbScript := preload("res://content/content_db.gd")

# The shared paperdoll widget (#639) — preloaded by path for the world-isolation regression
# lock (#643 bleed fix), same no-autoload reason as above.
const PaperdollScript := preload("res://scenes/charselect/character_paperdoll.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


const CHIBI_FIXTURE_DIR: String = "user://charcreate_chibi_fixture"


func _initialize() -> void:
	print("meridian character-create view RUNTIME verify (#639)")
	await _verify_view()
	await _verify_world_isolation()
	await _verify_chibi_pack_driven()
	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


# --- Chibi pack-driven char-create (story #760, design §8) --------------------
# The C7 proof in the REAL creation view: with a theme pack (6 colour races × 4 classes +
# appearance body_material) mounted, the race/class/sex pickers are PACK-DRIVEN (not the
# compiled MeridianRoster), the race picker IS the colour choice, and selecting a colour
# race assembles the COLOURED body (body_material dye applied) — no capsule fallback. The
# fixture reuses the real staged core body + masks + russet dye so the preview loads real
# bytes; the human UI-E2E covers the actual chibi art.
func _verify_chibi_pack_driven() -> void:
	print(" chibi pack-driven char-create (#760, design §8):")
	_write_chibi_fixture()
	# Mount the theme pack into the shared ContentDB the view reads (restored to core below).
	var db = ContentDbScript.instance()
	var loaded: bool = db.load_from(CHIBI_FIXTURE_DIR)
	_check("chibi-shape theme pack mounts", loaded and db.races().size() == 6)

	var packed: PackedScene = load("res://scenes/charselect/character_create.tscn")
	root.size = Vector2i(1728, 972)
	var view := packed.instantiate()
	root.add_child(view)
	await process_frame
	await process_frame

	var race_opt: OptionButton = view.find_child("RaceOption", true, false)
	var class_opt: OptionButton = view.find_child("ClassOption", true, false)
	var sex_opt: OptionButton = view.find_child("SexOption", true, false)

	# Pickers are pack-driven: 6 colour races (ids 1..6), 4 classes, male/female sex.
	_check("race picker is PACK-DRIVEN (6 colour races, not the compiled 4)",
		race_opt != null and race_opt.item_count == 6)
	_check("race picker items carry roster ids 1..6 with colour names",
		race_opt != null and race_opt.get_item_id(0) == 1 and race_opt.get_item_id(5) == 6
		and race_opt.get_item_text(0) == "Red" and race_opt.get_item_text(4) == "Gold")
	_check("class picker is pack-driven (4 classes)", class_opt != null and class_opt.item_count == 4)
	_check("sex picker offers male/female", sex_opt != null and sex_opt.item_count == 2)

	# The default selection assembles a real coloured body (the FIRST colour race, Red).
	var default_body: Node = view.find_child("PreviewBody", true, false)
	_check("default colour race assembles an AssembledCharacter (no capsule fallback)",
		default_body != null and default_body.has_method("body_skeleton"))

	# The race picker IS the colour choice: select Gold (id 5, the metallic race) and the
	# preview re-assembles the coloured body with the dye ShaderMaterial on its geosets.
	race_opt.select(race_opt.get_item_index(5))
	race_opt.item_selected.emit(race_opt.get_item_index(5))
	await process_frame
	var gold_body: Node = view.find_child("PreviewBody", true, false)
	_check("selecting a colour race assembles the coloured body (AssembledCharacter)",
		gold_body != null and gold_body.has_method("body_geosets"))
	var dyed := false
	if gold_body != null and gold_body.has_method("body_geosets"):
		for g in gold_body.body_geosets():
			if g is MeshInstance3D and g.material_override is ShaderMaterial \
					and (g.material_override as ShaderMaterial).shader != null \
					and (g.material_override as ShaderMaterial).shader.resource_path \
						== "res://characters/dye_tint.gdshader":
				dyed = true
				break
	_check("the coloured body carries the body_material dye shader on its geosets", dyed)

	view.queue_free()
	# Restore the staged core pack so nothing after this verify sees the fixture.
	db.load_from("res://meridian/core")


# Write the chibi-shape theme pack: 6 colour races + 4 classes + per-race appearance
# body_material, reusing the real staged core body + masks + russet dye (same reuse pattern
# as assembled_character_verify) so the preview loads real bytes.
func _write_chibi_fixture() -> void:
	DirAccess.make_dir_recursive_absolute(CHIBI_FIXTURE_DIR)
	var contents: PackedStringArray = [
		JSON.stringify({"id": "core:art.char.ardent.male.base", "numeric_id": 81,
			"resource": "res://meridian/core/art/char/ardent/male/base.scn", "hash": ""}),
		JSON.stringify({"id": "core:art.item.armor.warden_chest_mask", "numeric_id": 91,
			"resource": "res://meridian/core/art/item/armor/warden_chest_mask.res", "hash": ""}),
		JSON.stringify({"id": "core:art.item.armor.warden_head_mask", "numeric_id": 97,
			"resource": "res://meridian/core/art/item/armor/warden_head_mask.res", "hash": ""}),
		JSON.stringify({"id": "core:dye.russet", "numeric_id": 78,
			"resource": "res://meridian/core/tables/dye.bin", "hash": ""}),
	]
	var jsonl := FileAccess.open(CHIBI_FIXTURE_DIR + "/pack.contents.jsonl", FileAccess.WRITE)
	jsonl.store_string("\n".join(contents) + "\n")
	jsonl.close()

	var race_names: Array = ["Red", "Green", "Blue", "Yellow", "Gold", "Silver"]
	var race_rows: Array = []
	var appearance_rows: Array = []
	for i in range(6):
		var rid: int = i + 1
		var rname: String = String(race_names[i]).to_lower()
		var metallic: float = 1.0 if rid >= 5 else 0.0
		race_rows.append({"id": "chibi:race.%s" % rname, "numeric_id": 1000 + rid,
			"roster_id": rid, "name": race_names[i]})
		var bm: Dictionary = {
			"albedo": "core:art.item.armor.warden_head_mask",
			"dye_mask": "core:art.item.armor.warden_chest_mask",
			"metallic": metallic,
			"dyes": [{"channel": "primary", "dye": "core:dye.russet"}],
		}
		# Both sexes share the body (design §8) — emit male + female, same body_material.
		for sex_name in ["male", "female"]:
			appearance_rows.append({
				"id": "chibi:appearance.%s.%s" % [rname, sex_name],
				"numeric_id": 1100 + rid * 2 + (0 if sex_name == "male" else 1),
				"race": rname, "sex": sex_name,
				"skeleton": "core:art.char.ardent.male.base",
				"body_model": "core:art.char.ardent.male.base",
				"body_material": bm,
				"presets": {"hair": [], "face": [], "skin": []},
			})

	var class_names: Array = ["Warrior", "Mage", "Rogue", "Priest"]
	var class_rows: Array = []
	for i in range(4):
		class_rows.append({"id": "chibi:class.%s" % String(class_names[i]).to_lower(),
			"numeric_id": 1200 + i + 1, "roster_id": i + 1, "name": class_names[i]})

	var data: Dictionary = {
		"schema": "meridian/pack-data@1", "namespace": "chibi",
		"appearance": appearance_rows, "class": class_rows,
		"dye": [{"id": "core:dye.russet", "numeric_id": 78, "color": "#8a4b2d"}],
		"item": [], "race": race_rows,
	}
	var json := FileAccess.open(CHIBI_FIXTURE_DIR + "/pack.data.json", FileAccess.WRITE)
	json.store_string(JSON.stringify(data, "  "))
	json.close()


# #643 BLEED REGRESSION LOCK: mounting TWO paperdolls at once (the roster view's + the
# creation view's) must NOT make them share a 3D world. A SubViewport defaults to
# own_world_3d = false, which SHARES the parent viewport's World3D — so both mounted
# PreviewBody characters coexisted in one world and each viewport's camera rendered BOTH
# (the roster's selected character bled a static second model into the creation view).
# The fix: character_paperdoll owns its World3D. This test builds a roster paperdoll AND a
# creation paperdoll with DIFFERENT races and proves their SubViewports each own a DISTINCT
# World3D (and neither shares the SceneTree root's world), with exactly ONE PreviewBody per
# world — the creation viewport must NOT contain the roster's body. FAILS on the pre-fix
# shared-world code, PASSES after.
func _verify_world_isolation() -> void:
	print(" paperdoll world isolation (two paperdolls mounted at once, #643 bleed):")

	# Mount both widgets under the same SceneTree — the exact condition that triggered the
	# bleed (pre-fix they'd share root's World3D).
	var roster_pd := PaperdollScript.new()          # roster view's preview (front-facing)
	root.add_child(roster_pd)
	var create_pd := PaperdollScript.new()          # creation view's preview (drag-to-rotate)
	create_pd.draggable = true
	root.add_child(create_pd)
	await process_frame  # let _ready() build each SubViewport + PreviewRoot

	# Different races so the two previews are genuinely distinct models (Dolmen vs Ardent).
	roster_pd.set_appearance(2, MeridianAppearance.default_appearance(), [])   # Dolmen
	create_pd.set_appearance(MeridianRoster.DEFAULT_RACE_ID,
		MeridianAppearance.default_appearance(), [])                           # Ardent
	await process_frame

	var roster_vp: SubViewport = roster_pd.get_child(0) if roster_pd.get_child_count() > 0 else null
	var create_vp: SubViewport = create_pd.get_child(0) if create_pd.get_child_count() > 0 else null
	_check("both paperdolls built a SubViewport",
		roster_vp is SubViewport and create_vp is SubViewport)

	# find_world_3d() returns the World3D a viewport ACTUALLY renders into: its own when
	# own_world_3d is set, else the one it inherits by walking up to the parent viewport.
	# (Godot exposes it here rather than get_world_3d(), which is lazily null under --headless
	# until a frame draws.) Pre-fix (own_world_3d = false) BOTH walk up to the SceneTree root's
	# shared world and are the SAME object; the fix gives each its own.
	var roster_world: World3D = roster_vp.find_world_3d() if roster_vp != null else null
	var create_world: World3D = create_vp.find_world_3d() if create_vp != null else null
	var root_world: World3D = root.get_world_3d()
	# THE lock: distinct World3D (with distinct RenderingServer scenarios) between the two
	# previews — pre-fix they resolved to the SAME shared world, so each camera drew BOTH.
	_check("roster + creation paperdolls render into DISTINCT World3D (no shared 3D scene)",
		roster_world != null and create_world != null
		and roster_world != create_world and roster_world.scenario != create_world.scenario)
	# And neither shares the SceneTree root's world — each preview is fully isolated.
	_check("neither paperdoll shares the SceneTree root's World3D",
		roster_world != root_world and create_world != root_world)

	# Exactly ONE PreviewBody per viewport, and the creation viewport does NOT contain the
	# roster's (Dolmen) body — proves no cross-viewport model bleed at the scene level too.
	var roster_bodies := roster_vp.find_children("PreviewBody", "", true, false) if roster_vp != null else []
	var create_bodies := create_vp.find_children("PreviewBody", "", true, false) if create_vp != null else []
	_check("roster viewport contains exactly one PreviewBody", roster_bodies.size() == 1)
	_check("creation viewport contains exactly one PreviewBody", create_bodies.size() == 1)
	_check("creation viewport does NOT contain the roster's PreviewBody (no bleed)",
		create_bodies.size() == 1 and roster_bodies.size() == 1
		and not create_bodies.has(roster_bodies[0]))

	roster_pd.queue_free()
	create_pd.queue_free()


func _verify_view() -> void:
	print(" character_create scene (isolated, no session):")
	var packed: PackedScene = load("res://scenes/charselect/character_create.tscn")
	_check("character_create.tscn loads", packed != null)
	if packed == null:
		return

	# Drive layout at the #630 window (1728x972) so the paperdoll gets a REAL on-screen
	# size to fill — the #643 fix ties the SubViewport render size to that, not to the
	# small custom_minimum_size floor.
	root.size = Vector2i(1728, 972)

	var view := packed.instantiate()
	root.add_child(view)
	await process_frame  # let _ready() run (builds the paperdoll, populates pickers)
	await process_frame  # let the Control layout settle so the paperdoll fills its panel

	var race_opt: OptionButton = view.find_child("RaceOption", true, false)
	var class_opt: OptionButton = view.find_child("ClassOption", true, false)
	var hair_opt: OptionButton = view.find_child("HairOption", true, false)
	var face_opt: OptionButton = view.find_child("FaceOption", true, false)
	var skin_opt: OptionButton = view.find_child("SkinOption", true, false)
	var name_edit: LineEdit = view.find_child("NameEdit", true, false)
	var preview: Control = view.find_child("PreviewHolder", true, false)

	_check("race picker populated from roster (4 items)", race_opt != null and race_opt.item_count == 4)
	_check("class picker populated from roster (4 items)", class_opt != null and class_opt.item_count == 4)
	_check("race picker item ids are roster ids",
		race_opt != null and race_opt.get_item_id(0) == 1 and race_opt.get_item_id(3) == 4)
	_check("name field caps at 32 characters (max_length)",
		name_edit != null and name_edit.max_length == 32)

	# Appearance pickers are CATALOG-DRIVEN off MeridianContentDB (#477, spec ② §3): the
	# default race (Ardent) has a mounted catalog, so the pickers reflect its preset lists
	# and the item ids are the stable preset ints the server validates.
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
	_check("view reports the selected appearance record",
		view._selected_appearance()["version"] == MeridianAppearance.VERSION
		and int(view._selected_appearance()["hair"]) == MeridianAppearance.DEFAULT_HAIR_ID)

	# #590 REGRESSION LOCK: the create UI must expose EVERY catalog preset, not a hardcoded
	# 1-3 range. The S6 catalog upgrade added hair/face preset id 4 — reachable only via the
	# catalog-driven pickers. Counts follow the ardent catalog (4 hair, 4 face, 3 skin).
	_check("hair picker exposes ALL catalog presets incl. id 4 (4 options, id 4 present)",
		hair_opt != null and hair_opt.item_count == 4 and hair_opt.get_item_index(4) != -1)
	_check("face picker exposes ALL catalog presets incl. id 4 (4 options, id 4 present)",
		face_opt != null and face_opt.item_count == 4 and face_opt.get_item_index(4) != -1)
	_check("skin picker exposes ALL catalog presets (3 options)",
		skin_opt != null and skin_opt.item_count == 3)
	# Selecting the formerly-unreachable id 4 flows into the create record verbatim.
	hair_opt.select(hair_opt.get_item_index(4))
	face_opt.select(face_opt.get_item_index(4))
	var look4: Dictionary = view._selected_appearance()
	_check("selecting hair/face preset id 4 yields a create record with hair=face=4",
		int(look4["hair"]) == 4 and int(look4["face"]) == 4)
	hair_opt.select(hair_opt.get_item_index(MeridianAppearance.DEFAULT_HAIR_ID))
	face_opt.select(face_opt.get_item_index(MeridianAppearance.DEFAULT_FACE_ID))

	# The large paperdoll live-updates: an AssembledCharacter mounts as PreviewBody for the
	# default (ardent) race — the SAME assembly node the world builds, not a bare capsule.
	var preview_body: Node = view.find_child("PreviewBody", true, false)
	_check("creation paperdoll built into the holder (SubViewport surface)",
		preview != null and preview.get_child_count() > 0
		and preview.get_child(0) is SubViewportContainer)

	# #643: the paperdoll must be LARGE and fill its panel — NOT pinned at the small
	# custom_minimum_size (360x460) it used to render at. At the 1728x972 window it should
	# fill roughly the left half of the Body and most of the vertical space, and the
	# SubViewport RENDER size must TRACK that real on-screen size (drawn large + sharp, not
	# an upscaled 360x460 surface).
	var paperdoll: SubViewportContainer = preview.get_child(0)
	var pd_vp: SubViewport = paperdoll.get_child(0) if paperdoll.get_child_count() > 0 else null
	_check("creation paperdoll fills its panel (on-screen size >> old 360x460 floor)",
		paperdoll.size.x >= 500.0 and paperdoll.size.y >= 500.0)
	_check("creation paperdoll SubViewport render size tracks the on-screen size (#643)",
		pd_vp != null
		and absi(pd_vp.size.x - int(round(paperdoll.size.x))) <= 2
		and absi(pd_vp.size.y - int(round(paperdoll.size.y))) <= 2
		and pd_vp.size.x >= 500 and pd_vp.size.y >= 500)
	_check("paperdoll mounts an AssembledCharacter for the default race",
		preview_body != null and preview_body.has_method("body_skeleton")
		and preview_body.has_method("applied_preset"))

	# Race #2 (Dolmen) D3: selecting Dolmen drives the pickers off the dolmen/male catalog
	# (not content-missing, not ardent) and assembles the DOLMEN body.
	var dolmen_cat: Dictionary = ContentDbScript.instance().catalog(2, 0)
	_check("dolmen catalog is mounted (Race #2 D3 — catalog(2,0) non-empty)",
		not dolmen_cat.is_empty())
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
	var dolmen_body: Node = view.find_child("PreviewBody", true, false)
	_check("selecting Dolmen assembles the Dolmen body (not a capsule fallback)",
		dolmen_body != null and dolmen_body.has_method("body_skeleton")
		and dolmen_body.body_skeleton() != null)

	# Content-missing (spec §6): a race with NO catalog (Sylvane, id 3) disables the pickers
	# with a visible "(content missing)" state, and the record falls back to the default.
	race_opt.select(race_opt.get_item_index(3))
	race_opt.item_selected.emit(race_opt.get_item_index(3))
	_check("no-catalog race disables the appearance pickers",
		hair_opt != null and hair_opt.disabled and face_opt.disabled and skin_opt.disabled)
	_check("no-catalog race shows a visible 'content missing' item",
		hair_opt != null and hair_opt.item_count == 1
		and hair_opt.get_item_text(0) == "(content missing)")
	_check("content-missing create record falls back to the default appearance",
		int(view._selected_appearance()["hair"]) == MeridianAppearance.DEFAULT_HAIR_ID)
	# Restore the M1-playable race for the signal-payload checks below.
	race_opt.select(race_opt.get_item_index(MeridianRoster.DEFAULT_RACE_ID))
	race_opt.item_selected.emit(race_opt.get_item_index(MeridianRoster.DEFAULT_RACE_ID))

	# --- Signals + local validation (the whole point: net-free, controller drives the wire) -
	# NB: a GDScript lambda captures LOCALS by copy — reassigning a captured local does not
	# propagate out. So we mutate a captured Dictionary/Array IN PLACE (by reference) to
	# record the payload, rather than reassigning `confirmed`.
	var confirmed: Dictionary = {}
	var confirm_count := [0]
	view.character_confirmed.connect(
		func(n: String, r: int, c: int, a: Dictionary) -> void:
			confirmed["name"] = n
			confirmed["race"] = r
			confirmed["class"] = c
			confirmed["appearance"] = a
			confirm_count[0] += 1)
	var cancelled := [false]
	view.creation_cancelled.connect(func() -> void: cancelled[0] = true)

	# Local name validation: an empty name surfaces an error and does NOT emit confirm.
	name_edit.text = "   "
	view._on_create_pressed()
	var err_label: Label = view.find_child("ErrorLabel", true, false)
	_check("empty name surfaces a local error and does NOT confirm",
		confirm_count[0] == 0 and err_label != null and not err_label.text.is_empty())

	# A valid create emits character_confirmed with the chosen name/race/class/appearance.
	class_opt.select(class_opt.get_item_index(2))  # Runcaller
	name_edit.text = "Kaelith"
	view._on_create_pressed()
	_check("Create emits character_confirmed exactly once", confirm_count[0] == 1)
	_check("character_confirmed carries the trimmed name",
		String(confirmed.get("name", "")) == "Kaelith")
	_check("character_confirmed carries the selected race + class ids",
		int(confirmed.get("race", 0)) == MeridianRoster.DEFAULT_RACE_ID
		and int(confirmed.get("class", 0)) == 2)
	_check("character_confirmed carries a v1 appearance record",
		int((confirmed.get("appearance", {}) as Dictionary).get("version", 0)) == MeridianAppearance.VERSION)

	# Cancel emits creation_cancelled (and no extra confirm).
	view._on_cancel_pressed()
	_check("Cancel emits creation_cancelled", cancelled[0])
	_check("Cancel does not emit a create", confirm_count[0] == 1)

	# reset() clears the name + error (the controller calls it on each (re)show).
	view.show_error("stale message")
	view.reset()
	_check("reset() clears the name field and the error label",
		name_edit.text.is_empty() and err_label.text.is_empty())

	view.queue_free()
