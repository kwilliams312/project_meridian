# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for AssembledCharacter
# (story #540, client-assembler spec ② §4/§6). NOT a shipped scene: a SceneTree
# script run
#   godot --headless --path client/project --script res://characters/assembled_character_verify.gd
# (same convention as content/content_db_verify.gd) so CI / a dev box can prove,
# with no render and no server, that the assembler builds the REAL staged
# blockout+pickaxe content correctly and that every spec-§6 fallback path holds.
#
# Two phases:
#   A. The staged core pack (res://meridian/core): body instanced (8 geosets
#      visible, 63 canonical bones incl. socket_*), pickaxe → BoneAttachment3D
#      on socket_main_hand with the weapon mesh child, incremental
#      set_equipment_slot idempotence, ⑤/S3 mask-tint dye on the Warden's Cuirass
#      (russet on channel 0 → dye_tint.gdshader ShaderMaterial) + unknown-dye and
#      non-dyeable-piece fallbacks, unknown-preset → preset 1, assemble() false on
#      a catalog miss.
#   B. A fixture pack (user://, same artifact shapes, models resolving to the
#      REAL staged art): worn.hides ["feet"] → geo_feet_lod0 hidden + restored
#      on unequip, race_overrides wholesale substitution, skinned gear
#      re-parented onto the body skeleton binding by bone name, missing worn
#      model → piece hidden + assembly_failed.
#
# Exits 0 on success, 1 on any failed assertion.

extends SceneTree

# Standalone --script mode never initializes autoloads — preload the scripts and
# use the shared MeridianContentDB instance() (content_db.gd ACCESS PATTERN).
const ContentDbScript := preload("res://content/content_db.gd")
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")

const FIXTURE_DIR: String = "user://assembler_fixture"

# The 8 blockout geoset regions (tools/blender/meridian_rig generate_blockout.py).
const REGIONS: Array = [
	"feet", "forearms", "hands", "head", "hips_legs", "lower_legs", "torso", "waist",
]

# Opaque wire slot ids for the checks (the assembler treats slots as keys).
const SLOT_MAIN_HAND: int = 1
const SLOT_CHEST: int = 5
const SLOT_FEET: int = 8

# Fixture item numeric ids (far above the staged pack's IF-9 range).
const FX_BOOTS: int = 900001
const FX_OVERRIDE: int = 900002
const FX_SKINNED: int = 900003
const FX_MISSING_MODEL: int = 900004
const FX_HIDE_MISSING: int = 900005  # declares models + hides, but the model is missing (T4 ruling)

var _fails := 0
var _failures: Array = []  # captured assembly_failed reasons, in emit order


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _on_assembly_failed(reason: String) -> void:
	_failures.append(reason)


func _initialize() -> void:
	print("meridian AssembledCharacter RUNTIME verify (#540)")

	var db = ContentDbScript.instance()
	_check("staged core pack is loaded", db.is_loaded())

	var ac = AssembledCharacterScript.new()
	ac.name = "assembled_character_under_test"
	root.add_child(ac)
	ac.assembly_failed.connect(_on_assembly_failed)

	# --- Phase A: the real staged blockout + pickaxe content -------------------
	_verify_body(ac)
	var pickaxe: int = db.numeric_id_for("core:item.rusty_pickaxe")
	_check("rusty_pickaxe resolves to a numeric id", pickaxe != 0)
	_verify_pickaxe_socket(ac, pickaxe)
	_verify_idempotence(ac, pickaxe)
	_verify_dye(ac, db, pickaxe)
	_verify_unknown_preset(ac)
	_verify_catalog_miss(ac)

	# --- Phase B: fixture pack (hides / race_overrides / skinned / missing) ----
	_verify_fixtures(ac, db)

	# Restore the real pack so anything after this verify sees loaded content.
	db.load_from("res://meridian/core")
	root.remove_child(ac)
	ac.free()

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


# --- A1. body: 8 geosets visible, 63 canonical bones, sockets ------------------
func _verify_body(ac) -> void:
	print(" assemble(1, 0) — ardent male body from the staged pack:")
	var ok: bool = ac.assemble(1, 0, {"hair": 1, "face": 1, "skin": 1}, [])
	_check("assemble returns true", ok and ac.is_assembled())
	var skel: Skeleton3D = ac.body_skeleton()
	_check("single Skeleton3D present", skel != null)
	_check("skeleton has the 63 canonical bones", skel != null and skel.get_bone_count() == 63)
	_check("socket_main_hand bone present", skel != null and skel.find_bone("socket_main_hand") >= 0)
	_check("socket_back bone present", skel != null and skel.find_bone("socket_back") >= 0)
	var visible_count: int = 0
	for region in REGIONS:
		var g: MeshInstance3D = ac.geoset_node(region)
		if g != null and g.visible:
			visible_count += 1
	_check("all 8 geoset meshes instanced and visible", visible_count == 8)

	# LOD-aware visibility (⑤/S4 real body ships an authored LOD0-3 chain): only
	# the 8 LOD0 geosets render; the lod1-3 meshes exist but stay hidden for the
	# deferred distance-based LOD system. Without this every character would draw
	# 4x its geometry (all LODs stacked) — the regression the S4 GPU render found.
	var all_geosets: Array = ac.body_geosets()
	_check("body ships the full geoset LOD chain (8 regions x 4 LODs = 32)",
		all_geosets.size() == 32)
	var vis: int = 0
	var hidden_lodn: int = 0
	var all_visible_are_lod0: bool = true
	for g2 in all_geosets:
		if not (g2 is MeshInstance3D):
			continue
		var nm: String = String(g2.name)
		var is_lod0: bool = not nm.contains("_lod") or nm.ends_with("_lod0")
		if g2.visible:
			vis += 1
			if not is_lod0:
				all_visible_are_lod0 = false
		elif not is_lod0:
			hidden_lodn += 1
	_check("exactly 8 geoset meshes are VISIBLE (LOD0 only)", vis == 8)
	_check("every visible geoset is LOD0", all_visible_are_lod0)
	_check("the 24 lod1-3 geosets exist but are hidden", hidden_lodn == 24)


# --- A2. pickaxe → BoneAttachment3D on socket_main_hand ------------------------
func _verify_pickaxe_socket(ac, pickaxe: int) -> void:
	print(" set_equipment_slot(main_hand, rusty_pickaxe):")
	ac.set_equipment_slot(SLOT_MAIN_HAND, pickaxe, [])
	var nodes: Array = ac.equipped_nodes(SLOT_MAIN_HAND)
	_check("pickaxe mounts exactly one piece", nodes.size() == 1)
	if nodes.size() != 1:
		return
	var attachment = nodes[0]
	_check("piece is a BoneAttachment3D on socket_main_hand",
		attachment is BoneAttachment3D and String(attachment.bone_name) == "socket_main_hand")
	_check("attachment is parented under the body skeleton",
		attachment.get_parent() == ac.body_skeleton())
	var weapon_meshes: Array = attachment.find_children("*", "MeshInstance3D", true, false)
	_check("weapon mesh child present under the attachment", not weapon_meshes.is_empty())
	_check("attachment records the worn model id",
		String(attachment.get_meta("model_id", "")) == "core:art.item.weapon.pickaxe_rusty")


# --- A3. incremental set_equipment_slot is idempotent --------------------------
func _verify_idempotence(ac, pickaxe: int) -> void:
	print(" set_equipment_slot idempotence (re-equip the same item):")
	var skel: Skeleton3D = ac.body_skeleton()
	var children_before: int = skel.get_child_count()
	ac.set_equipment_slot(SLOT_MAIN_HAND, pickaxe, [])
	_check("re-equip does not accumulate nodes",
		skel.get_child_count() == children_before
		and ac.equipped_nodes(SLOT_MAIN_HAND).size() == 1)
	# Unequip → the piece is gone from the skeleton entirely.
	ac.set_equipment_slot(SLOT_MAIN_HAND, 0, [])
	_check("unequip removes the piece",
		ac.equipped_nodes(SLOT_MAIN_HAND).is_empty()
		and skel.get_child_count() == children_before - 1)


# --- A4. ⑤/S3 mask-tint dye: per-channel ShaderMaterial + fallbacks ------------
# Equips the REAL staged Warden's Cuirass (a plate with an RGB dye mask +
# worn.dye_channels [primary, secondary]) and drives the new wire shape
# dyes:[{channel, dye_id}]. Proves: a known dye on channel 0 yields a
# ShaderMaterial (dye_tint.gdshader) whose primary colour param == the russet
# colour and whose mask is bound; an unknown dye id keeps the authored colours;
# a piece with NO dye_channels (the pickaxe) never tints even with a valid dye.
func _verify_dye(ac, db, pickaxe: int) -> void:
	print(" ⑤/S3 mask-tint dye — Warden's Cuirass, per-channel + fallbacks:")
	var russet: int = db.numeric_id_for("core:dye.russet")
	_check("russet resolves to a numeric id", russet != 0)
	var russet_color: Color = db.dye_color(russet)
	var chest: int = db.numeric_id_for("core:item.warden_chest")
	_check("warden_chest resolves to a numeric id", chest != 0)

	ac.set_equipment_slot(SLOT_CHEST, chest, [{"channel": 0, "dye_id": russet}])
	var mesh: MeshInstance3D = _first_piece_mesh(ac, SLOT_CHEST)
	_check("dyed warden piece mounts a mesh", mesh != null)
	var mat: ShaderMaterial = null
	if mesh != null:
		mat = mesh.material_override as ShaderMaterial
	_check("known dye applies a ShaderMaterial override", mat != null)
	if mat != null:
		_check("the override uses the dye_tint mask shader",
			mat.shader != null
			and mat.shader.resource_path == "res://characters/dye_tint.gdshader")
		var primary = mat.get_shader_parameter("dye_primary")
		_check("primary-channel colour param == the russet dye colour",
			primary is Color and (primary as Color).is_equal_approx(russet_color))
		_check("primary channel is flagged active (use_primary == 1)",
			float(mat.get_shader_parameter("use_primary")) == 1.0)
		_check("secondary channel stays inactive (only channel 0 was dyed)",
			float(mat.get_shader_parameter("use_secondary")) == 0.0)
		_check("the piece's dye mask texture is bound",
			mat.get_shader_parameter("dye_mask") is Texture2D)

	# Unknown dye id → authored colours (no override), assembly_failed once.
	ac.set_equipment_slot(SLOT_CHEST, chest, [{"channel": 0, "dye_id": 999999999}])
	mesh = _first_piece_mesh(ac, SLOT_CHEST)
	_check("unknown dye keeps the authored colours (no override)",
		mesh != null and mesh.material_override == null)
	_check("assembly_failed emitted for the unknown dye", _failures.has("dye:999999999"))
	ac.set_equipment_slot(SLOT_CHEST, 0, [])

	# A piece with no worn.dye_channels (the pickaxe) never tints, even with a
	# valid dye — the mask-tint path gates on the item declaring itself dyeable.
	ac.set_equipment_slot(SLOT_MAIN_HAND, pickaxe, [{"channel": 0, "dye_id": russet}])
	mesh = _first_piece_mesh(ac, SLOT_MAIN_HAND)
	_check("non-dyeable piece (no dye_channels) is never tinted",
		mesh != null and mesh.material_override == null)
	ac.set_equipment_slot(SLOT_MAIN_HAND, 0, [])


# --- A5. unknown preset id → catalog entry 1 + assembly_failed -----------------
func _verify_unknown_preset(ac) -> void:
	print(" unknown preset id — falls back to preset 1:")
	var ok: bool = ac.assemble(1, 0, {"hair": 99, "face": 1, "skin": 1}, [])
	_check("assemble still succeeds", ok)
	var hair: Dictionary = ac.applied_preset("hair")
	_check("hair preset fell back to catalog entry 1", int(hair.get("id", 0)) == 1)
	_check("assembly_failed emitted once for the unknown preset",
		_failures.count("preset:hair:99") == 1)
	var face: Dictionary = ac.applied_preset("face")
	_check("known face preset applied verbatim", int(face.get("id", 0)) == 1)


# --- A6. catalog miss → assemble() returns false (capsule fallback) ------------
func _verify_catalog_miss(ac) -> void:
	print(" catalog miss — race with no catalog (Dolmen, id 2):")
	var ok: bool = ac.assemble(2, 0, {}, [])
	_check("assemble returns false", not ok and not ac.is_assembled())
	_check("assembly_failed names the missing catalog", _failures.has("catalog:2|0"))


# --- B. fixture pack: hides / race_overrides / skinned / missing model ---------
func _verify_fixtures(ac, db) -> void:
	print(" fixture pack (user://) — hides, race_overrides, skinned re-parent:")
	_write_fixture_pack()
	var loaded: bool = db.load_from(FIXTURE_DIR)
	_check("fixture pack loads", loaded)
	var ok: bool = ac.assemble(1, 0, {}, [])
	_check("assemble against the fixture pack", ok)

	# worn.hides ["feet"] → the feet region hidden at every LOD; unequip → restored.
	var visible_before: int = _visible_geoset_count(ac)
	_check("body-only assemble shows 8 visible geosets", visible_before == 8)
	ac.set_equipment_slot(SLOT_FEET, FX_BOOTS, [])
	var feet: MeshInstance3D = ac.geoset_node("feet")
	_check("hides ['feet'] → geo_feet_lod0 hidden", feet != null and not feet.visible)
	# Region hide is LOD-aware: every LOD of the hidden region goes dark, not just
	# LOD0 (otherwise the region's lod1-3 would still draw under the boot).
	var feet_lods_hidden: bool = true
	for g3 in ac.body_geosets():
		if g3 is MeshInstance3D and String(g3.name).begins_with("geo_feet_") and g3.visible:
			feet_lods_hidden = false
	_check("hides ['feet'] hides ALL feet LODs (geo_feet_lod0..3)", feet_lods_hidden)
	_check("region-hiding item drops the visible geoset count (8 -> 7)",
		_visible_geoset_count(ac) == visible_before - 1)
	var others_visible: bool = true
	for region in REGIONS:
		if region == "feet":
			continue
		var g: MeshInstance3D = ac.geoset_node(region)
		if g == null or not g.visible:
			others_visible = false
	_check("the other 7 geosets stay visible", others_visible)
	ac.set_equipment_slot(SLOT_FEET, 0, [])
	_check("unequip restores geo_feet_lod0", feet != null and feet.visible)
	_check("unequip restores the visible geoset count to 8",
		_visible_geoset_count(ac) == visible_before)

	# race_overrides: the ardent override substitutes models wholesale — the
	# bogus default model must never be touched.
	ac.set_equipment_slot(SLOT_MAIN_HAND, FX_OVERRIDE, [])
	var nodes: Array = ac.equipped_nodes(SLOT_MAIN_HAND)
	_check("race_overrides picks the override model path",
		nodes.size() == 1
		and String(nodes[0].get_meta("model_id", "")) == "core:art.item.weapon.pickaxe_rusty")
	_check("the bogus default model was never loaded",
		not _failures.has("model:core:art.fixture.bogus_default"))
	ac.set_equipment_slot(SLOT_MAIN_HAND, 0, [])

	# Skinned (socketless) gear: meshes re-parent onto the body skeleton and
	# bind by bone name. The fixture reuses the real ardent body model, so the
	# piece's mesh count tracks that model's geoset+LOD count — assert the
	# BEHAVIOR (re-parented + bound by bone name), never a magic count, so this
	# never breaks again when the model's mesh count changes.
	var skel: Skeleton3D = ac.body_skeleton()
	ac.set_equipment_slot(SLOT_CHEST, FX_SKINNED, [])
	var gear: Array = ac.equipped_nodes(SLOT_CHEST)
	_check("skinned gear mounts at least one mesh", gear.size() > 0)
	var bound: bool = not gear.is_empty()
	for mi in gear:
		if not (mi is MeshInstance3D) or mi.get_parent() != skel \
				or mi.skeleton != NodePath("..") or mi.skin == null:
			bound = false
	_check("every re-parented mesh is under the body skeleton, bound by bone name",
		bound)
	_check("Skin bone names resolve on the body skeleton (no skin: failure)",
		not _failures.has("skin:core:art.char.ardent.male.base"))
	ac.set_equipment_slot(SLOT_CHEST, 0, [])

	# Missing worn model → the piece stays hidden + assembly_failed (spec §6).
	ac.set_equipment_slot(SLOT_MAIN_HAND, FX_MISSING_MODEL, [])
	_check("missing worn model → piece hidden (no nodes)",
		ac.equipped_nodes(SLOT_MAIN_HAND).is_empty())
	_check("assembly_failed names the missing model",
		_failures.has("model:core:art.fixture.no_such_model"))
	ac.set_equipment_slot(SLOT_MAIN_HAND, 0, [])

	# Lead ruling (②/T4, #541): an item that DECLARES models AND hides but whose model
	# fails to load must NOT hide the body region — otherwise the region is stripped while
	# its covering piece is invisible ("hide-while-uncovered"). Equip a chest item that
	# hides "torso" but whose only model is missing: the piece stays hidden (no nodes) AND
	# geo_torso_lod0 stays VISIBLE (contrast the pure-hide fixture_boots above, which DOES
	# hide because it authored NO model).
	var torso_before: MeshInstance3D = ac.geoset_node("torso")
	_check("torso geoset visible before the hide-missing item",
		torso_before != null and torso_before.visible)
	ac.set_equipment_slot(SLOT_CHEST, FX_HIDE_MISSING, [])
	_check("hide-missing item mounts no piece (its model failed to load)",
		ac.equipped_nodes(SLOT_CHEST).is_empty())
	var torso_after: MeshInstance3D = ac.geoset_node("torso")
	_check("torso geoset STAYS visible — no hide-while-uncovered (T4 ruling)",
		torso_after != null and torso_after.visible)
	ac.set_equipment_slot(SLOT_CHEST, 0, [])


# How many BODY geoset meshes are currently visible (LOD0-only at M1).
func _visible_geoset_count(ac) -> int:
	var n: int = 0
	for g in ac.body_geosets():
		if g is MeshInstance3D and g.visible:
			n += 1
	return n


# The first MeshInstance3D of the piece mounted in `slot` (null when none).
func _first_piece_mesh(ac, slot: int) -> MeshInstance3D:
	var nodes: Array = ac.equipped_nodes(slot)
	if nodes.is_empty():
		return null
	var piece = nodes[0]
	if piece is MeshInstance3D:
		return piece
	var meshes: Array = piece.find_children("*", "MeshInstance3D", true, false)
	if meshes.is_empty():
		return null
	return meshes[0]


# Write the fixture pack: the same artifact shapes emit-pck produces, with the
# fixture models resolving to the REAL staged art (res://meridian/core/...), so
# the loader path under test is identical to production.
func _write_fixture_pack() -> void:
	DirAccess.make_dir_recursive_absolute(FIXTURE_DIR)

	var contents: PackedStringArray = [
		JSON.stringify({"id": "core:art.char.ardent.male.base", "numeric_id": 81,
			"resource": "res://meridian/core/art/char/ardent/male/base.scn", "hash": ""}),
		JSON.stringify({"id": "core:art.item.weapon.pickaxe_rusty", "numeric_id": 13,
			"resource": "res://meridian/core/art/item/weapon/pickaxe_rusty.scn", "hash": ""}),
	]
	var jsonl := FileAccess.open(FIXTURE_DIR + "/pack.contents.jsonl", FileAccess.WRITE)
	jsonl.store_string("\n".join(contents) + "\n")
	jsonl.close()

	var preset_list: Array = [
		{"id": 1, "model": "core:art.char.ardent.male.base"},
		{"id": 2, "model": "core:art.char.ardent.male.base"},
	]
	var data: Dictionary = {
		"schema": "meridian/pack-data@1",
		"namespace": "core",
		"appearance": [{
			"id": "core:appearance.ardent.male", "numeric_id": 82,
			"race": "ardent", "sex": "male",
			"skeleton": "core:art.char.ardent.male.skeleton",
			"body_model": "core:art.char.ardent.male.base",
			"presets": {"hair": preset_list, "face": preset_list, "skin": preset_list},
		}],
		"dye": [],
		"item": [
			{"id": "core:item.fixture_boots", "numeric_id": FX_BOOTS, "worn": {
				"models": [], "hides": ["feet"], "attach": {},
				"dye_channels": [], "race_overrides": {}}},
			{"id": "core:item.fixture_override", "numeric_id": FX_OVERRIDE, "worn": {
				"models": [{"model": "core:art.fixture.bogus_default", "mirror": "none"}],
				"hides": [], "attach": {"socket": "main_hand"}, "dye_channels": [],
				"race_overrides": {"ardent": {
					"models": [{"model": "core:art.item.weapon.pickaxe_rusty", "mirror": "none"}],
					"hides": []}}}},
			{"id": "core:item.fixture_skinned", "numeric_id": FX_SKINNED, "worn": {
				"models": [{"model": "core:art.char.ardent.male.base", "mirror": "none"}],
				"hides": [], "attach": {}, "dye_channels": [], "race_overrides": {}}},
			{"id": "core:item.fixture_missing_model", "numeric_id": FX_MISSING_MODEL, "worn": {
				"models": [{"model": "core:art.fixture.no_such_model", "mirror": "none"}],
				"hides": [], "attach": {"socket": "main_hand"}, "dye_channels": [],
				"race_overrides": {}}},
			{"id": "core:item.fixture_hide_missing", "numeric_id": FX_HIDE_MISSING, "worn": {
				"models": [{"model": "core:art.fixture.no_such_model", "mirror": "none"}],
				"hides": ["torso"], "attach": {}, "dye_channels": [],
				"race_overrides": {}}},
		],
	}
	var json := FileAccess.open(FIXTURE_DIR + "/pack.data.json", FileAccess.WRITE)
	json.store_string(JSON.stringify(data, "  "))
	json.close()
