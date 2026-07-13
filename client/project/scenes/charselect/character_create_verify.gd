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

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian character-create view RUNTIME verify (#639)")
	await _verify_view()
	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


func _verify_view() -> void:
	print(" character_create scene (isolated, no session):")
	var packed: PackedScene = load("res://scenes/charselect/character_create.tscn")
	_check("character_create.tscn loads", packed != null)
	if packed == null:
		return

	var view := packed.instantiate()
	root.add_child(view)
	await process_frame  # let _ready() run (builds the paperdoll, populates pickers)

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
