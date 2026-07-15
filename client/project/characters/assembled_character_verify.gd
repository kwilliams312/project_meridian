# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for AssembledCharacter
# (story #540, client-assembler spec ② §4/§6; extended by Dolmen D4 #615). NOT a
# shipped scene: a SceneTree script run
#   godot --headless --path client/project --import   # once, to populate the
#   godot --headless --path client/project --script res://characters/assembled_character_verify.gd
# (same convention as content/content_db_verify.gd) so CI / a dev box can prove,
# with no render and no server, that the assembler builds the REAL staged
# blockout+pickaxe content correctly and that every spec-§6 fallback path holds.
#
# ⛔ Run `--import` FIRST. The assembler resolves the `MeridianRoster` global
# class, which lives only in .godot/global_script_class_cache.cfg — a fresh
# checkout has no cache, so `--script` alone would fail to find the class. A
# single headless `--import` pass builds the cache (and imports the staged art);
# after that this verify runs offline against bin/*.framework with no re-import.
#
# Three phases:
#   A. The staged core pack (res://meridian/core): body instanced (8 geosets
#      visible, 63 canonical bones incl. socket_*), pickaxe → BoneAttachment3D
#      on socket_main_hand with the weapon mesh child, incremental
#      set_equipment_slot idempotence, ⑤/S3 mask-tint dye on the Warden's Cuirass
#      (russet on channel 0 → dye_tint.gdshader ShaderMaterial) + unknown-dye and
#      non-dyeable-piece fallbacks, the full Ardent Warden's Kit composite,
#      unknown-preset → preset 1, assemble() false on a catalog miss.
#   A-D4. CROSS-RACE FIT-CHECK (Dolmen #615): assemble the Dolmen (race 2) with
#      the SAME Ardent-authored Warden's Kit + hair + iron sword, all russet-dyed,
#      and assert the gear binds cleanly by bone name onto the Dolmen's stockier
#      63-bone skeleton — every piece mounts, the hide union is correct, the sword
#      rides the Dolmen hand socket, and NO assembly_failed fires. This is the
#      standing regression guard behind the "model per race" proof: gear authored
#      once for Ardent deforms onto Dolmen with ZERO race_overrides.
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
# A CHIBI-SHAPE fixture (story #760, design 2026-07-14-chibi §8): a theme pack that ships
# a 6-colour-race roster + appearance body_material, reusing the REAL staged core art so
# the body-dye path actually loads meshes/textures. Proves the client wiring (pack roster
# drives the pickers, body_material resolves, the body assembles WITH the dye applied,
# zero assembly_failed) without needing the chibi art staged — the human UI-E2E covers the
# real chibi assets. Separate dir so it never clobbers the FIXTURE_DIR pack above.
const CHIBI_FIXTURE_DIR: String = "user://assembler_chibi_fixture"

# The 8 blockout geoset regions (tools/blender/meridian_rig generate_blockout.py).
const REGIONS: Array = [
	"feet", "forearms", "hands", "head", "hips_legs", "lower_legs", "torso", "waist",
]

# Opaque wire slot ids for the checks (the assembler treats slots as keys).
const SLOT_MAIN_HAND: int = 1
const SLOT_CHEST: int = 5
const SLOT_FEET: int = 8
# Additional distinct slot keys for the full Warden's Kit composite (⑤/S6). Values
# are opaque — the assembler keys _slots by them; realism is irrelevant here.
const SLOT_HEAD: int = 2
const SLOT_SHOULDERS: int = 3
const SLOT_HANDS: int = 6
const SLOT_LEGS: int = 7

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
	_verify_full_kit(ac, db)
	_verify_dolmen_fitcheck(ac, db)
	_verify_unknown_preset(ac)
	_verify_catalog_miss(ac)

	# --- Phase B: fixture pack (hides / race_overrides / skinned / missing) ----
	_verify_fixtures(ac, db)

	# --- Phase C: chibi-shape pack (roster + body_material body-dye, #760) ------
	_verify_chibi_body_material(ac, db)

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


# --- A4b. ⑤/S6 FULL-KIT composite proof: the whole Warden's Kit + a dye + hair --
# The S6 "no grey boxes" DoD, proven headlessly (the lead's GPU render proves it
# visually): assemble the ardent body with a real HAIR preset and equip ALL SIX
# real staged Warden's Kit slots at once, with a dye on the chest. Asserts every
# slot mounts a mesh; the hide UNION leaves only the two uncovered regions
# (forearms + waist) visible; the chest carries the mask-tint dye ShaderMaterial
# in the composite; and the real hair mesh is seated on the head. (The round-1
# shoulder-guard 'UpperArm bridge' was REMOVED in S6 round-2 and arms work is
# deferred by #593 — with the full kit the arms float, an accepted S6 limitation
# tracked in #587. This phase does NOT assert an arms bridge; see the NOTE below.)
func _verify_full_kit(ac, db) -> void:
	print(" ⑤/S6 full Warden's Kit composite — body + hair + 6 slots + dye:")
	var russet: int = db.numeric_id_for("core:dye.russet")
	var slot_items: Dictionary = {
		SLOT_HEAD: "core:item.warden_head",
		SLOT_SHOULDERS: "core:item.warden_shoulders",
		SLOT_CHEST: "core:item.warden_chest",
		SLOT_HANDS: "core:item.warden_hands",
		SLOT_LEGS: "core:item.warden_legs",
		SLOT_FEET: "core:item.warden_feet",
	}

	# Assemble with a REAL hair preset (⑤/S6 catalog now points hair at hair_2).
	var ok: bool = ac.assemble(1, 0, {"hair": 2, "face": 2, "skin": 2}, [])
	_check("full-kit assemble returns true", ok and ac.is_assembled())

	# Hair must (a) resolve to a real distinct model, (b) mount EXACTLY ONCE (the
	# render showed a doubled blob — guard against double-mount), and (c) SEAT ON
	# THE HEAD, not float above it (⑤/S6 defect). Render-independent seating check
	# per lead: the mounted hair MeshInstance's global-AABB centre Y must land in
	# the head band (head bone at Y≈1.52, head geoset spans ~1.40–1.72), NOT ~1.9+.
	var hair: Dictionary = ac.applied_preset("hair")
	var hair_model: String = String(hair.get("model", ""))
	_check("hair preset resolves to a real (non-body) hair model",
		hair_model != "" and hair_model != "core:art.char.ardent.male.base")
	var skel: Skeleton3D = ac.body_skeleton()
	var hair_meshes: Array = []
	if skel != null:
		for c in skel.find_children("*", "MeshInstance3D", true, false):
			if String(c.get_meta("model_id", "")) == hair_model:
				hair_meshes.append(c)
	_check("hair mounts EXACTLY ONE mesh (no double-mount)", hair_meshes.size() == 1)
	if hair_meshes.size() == 1:
		var hm: MeshInstance3D = hair_meshes[0]
		var gaabb: AABB = hm.global_transform * hm.get_aabb()
		var cy: float = gaabb.position.y + gaabb.size.y * 0.5
		_check("hair seats on the head (global-AABB centre Y %.3f in 1.40–1.75)" % cy,
			cy >= 1.40 and cy <= 1.75)

	# Equip all six kit slots; dye the chest on the primary channel.
	for slot in slot_items:
		var iid: int = db.numeric_id_for(slot_items[slot])
		_check("%s resolves to a numeric id" % slot_items[slot], iid != 0)
		var dyes: Array = []
		if slot == SLOT_CHEST:
			dyes = [{"channel": 0, "dye_id": russet}]
		ac.set_equipment_slot(slot, iid, dyes)

	# Every slot mounts at least one mesh (no invisible/greybox-missing piece).
	var all_slots_mounted: bool = true
	for slot in slot_items:
		if _first_piece_mesh(ac, slot) == null:
			all_slots_mounted = false
	_check("all six Warden's Kit slots mount a mesh", all_slots_mounted)

	# Hide union: the Warden's Kit hides head, torso (head+shoulders+chest all map
	# to torso/head), hands, hips_legs, feet — the union of the six pieces' worn.hides
	# (see content/core/items/warden_*.item.yaml). That leaves forearms, lower_legs
	# and waist visible: the three regions no kit piece covers (the shins between the
	# greaves and boots, the forearms below the vambraces, and the waist gap). This
	# is the "no bare grey body poking through the ARMOURED regions" invariant — the
	# uncovered regions are body skin by design. (Round-1's kit also hid lower_legs;
	# the #599 "Warden's Kit v2 polish — feet fit" re-cut stopped the boots hiding
	# the shin, so lower_legs is now an authored uncovered region.)
	var hidden_regions: Array = ["head", "torso", "hands", "hips_legs", "feet"]
	var visible_regions: Array = ["forearms", "lower_legs", "waist"]
	var hides_ok: bool = true
	for region in hidden_regions:
		var g: MeshInstance3D = ac.geoset_node(region)
		if g == null or g.visible:
			hides_ok = false
	_check("all five kit-covered body regions are hidden", hides_ok)
	var uncovered_ok: bool = true
	for region in visible_regions:
		var g2: MeshInstance3D = ac.geoset_node(region)
		if g2 == null or not g2.visible:
			uncovered_ok = false
	_check("the three uncovered regions (forearms, lower_legs, waist) stay visible", uncovered_ok)
	_check("exactly 3 body geosets visible under the full kit",
		_visible_geoset_count(ac) == 3)

	# The chest dye survives in the composite (mask-tint ShaderMaterial applied).
	var chest_mesh: MeshInstance3D = _first_piece_mesh(ac, SLOT_CHEST)
	var chest_mat: ShaderMaterial = null
	if chest_mesh != null:
		chest_mat = chest_mesh.material_override as ShaderMaterial
	_check("dyed chest carries the mask-tint ShaderMaterial in the full kit",
		chest_mat != null
		and chest_mat.shader != null
		and chest_mat.shader.resource_path == "res://characters/dye_tint.gdshader")

	# NOTE (⑤/S6 known limitation, tracked in #587): with the full kit the arms
	# float — the body's upper arm lives in the hidden `torso` geoset, orphaning
	# the `forearms` geoset. Not asserted here: the cure is a body geoset re-cut
	# (S4 territory), an accepted limitation for S6, not a regression this story
	# introduced.

	# Tear the kit back down so later phases start clean.
	for slot in slot_items:
		ac.set_equipment_slot(slot, 0, [])


# --- A-D4. CROSS-RACE FIT-CHECK (Dolmen #615): the "model per race" proof -------
# The standing regression guard behind the D4 GPU render. Assembles the DOLMEN
# (race 2, sex 0 — a DISTINCT stockier/shorter 63-bone skeleton + its own Meshy
# body) and equips the SAME Ardent-authored Warden's Kit + hair + iron sword, all
# russet-dyed. The kit was authored ONCE, on Ardent; it must deform onto Dolmen
# purely by bone-NAME binding (the two skeletons share bone names/hierarchy, only
# rest transforms differ — the shared-skeleton invariant). Asserts the whole
# composite lands with ZERO race_overrides: every armour piece + the sword mounts,
# the hide union is identical to Ardent's (hides are race-agnostic), the sword
# rides the Dolmen hand socket, and NO assembly_failed fires anywhere in the equip.
# 11 meshes render (3 uncovered body geosets + 6 armour + hair + sword), matching
# the lead's D4 GPU render. If this ever goes red, cross-race gear reuse regressed.
func _verify_dolmen_fitcheck(ac, db) -> void:
	print(" A-D4 cross-race fit-check — Dolmen (race 2) wears the Ardent Warden's Kit + sword:")
	var russet: int = db.numeric_id_for("core:dye.russet")
	_check("russet dye resolves", russet != 0)

	# Snapshot the failure log up front: the whole Dolmen assemble+equip must add
	# ZERO new assembly_failed reasons — that is the crux of the proof.
	var failures_before: int = _failures.size()

	# Assemble the Dolmen body from its OWN D3 catalog (ContentDB.catalog(2, 0)).
	var ok: bool = ac.assemble(2, 0, {"hair": 1, "face": 1, "skin": 1}, [])
	_check("assemble(2, 0) returns true — Dolmen has a real catalog now", ok and ac.is_assembled())
	var skel: Skeleton3D = ac.body_skeleton()
	_check("Dolmen mounts a single canonical Skeleton3D", skel != null)
	_check("Dolmen skeleton has exactly the 63 canonical bones", skel != null and skel.get_bone_count() == 63)
	_check("Dolmen skeleton keeps the socket_main_hand bone (sword mount)",
		skel != null and skel.find_bone("socket_main_hand") >= 0)

	# It must be the DOLMEN body, not the Ardent one (D3 catalog wiring).
	var body_model: String = String(db.catalog(2, 0).get("body_model", ""))
	_check("catalog(2, 0) resolves the Dolmen body model (not Ardent)",
		body_model == "core:art.char.dolmen.male.base")

	# Equip the full dyed Warden's Kit (6 armour slots, russet on channel 0 of each)
	# + the iron sword on the main hand. Ardent-authored item ids — reused verbatim.
	var armor_slots: Dictionary = {
		SLOT_HEAD: "core:item.warden_head",
		SLOT_SHOULDERS: "core:item.warden_shoulders",
		SLOT_CHEST: "core:item.warden_chest",
		SLOT_HANDS: "core:item.warden_hands",
		SLOT_LEGS: "core:item.warden_legs",
		SLOT_FEET: "core:item.warden_feet",
	}
	for slot in armor_slots:
		var iid: int = db.numeric_id_for(armor_slots[slot])
		_check("%s resolves" % armor_slots[slot], iid != 0)
		ac.set_equipment_slot(slot, iid, [{"channel": 0, "dye_id": russet}])
	var sword: int = db.numeric_id_for("core:item.iron_sword")
	_check("core:item.iron_sword resolves", sword != 0)
	ac.set_equipment_slot(SLOT_MAIN_HAND, sword, [])

	# CRUX: not a single assembly_failed across the whole Dolmen assemble + equip.
	# A missing skin bone, a failed model load, or an unresolved socket would show
	# up here — this is what "binds cleanly by bone name" means, mechanically.
	_check("ZERO assembly_failed across the Dolmen assemble + full-kit equip",
		_failures.size() == failures_before)

	# Every armour piece mounted a real mesh (no invisible/greybox-missing piece).
	var all_mounted: bool = true
	for slot in armor_slots:
		if _first_piece_mesh(ac, slot) == null:
			all_mounted = false
	_check("all six Ardent Warden's Kit pieces mount a mesh on the Dolmen", all_mounted)

	# The sword rides the Dolmen hand: a BoneAttachment3D on socket_main_hand,
	# parented under the Dolmen skeleton, carrying a weapon mesh child.
	var sword_nodes: Array = ac.equipped_nodes(SLOT_MAIN_HAND)
	_check("iron sword mounts exactly one piece", sword_nodes.size() == 1)
	if sword_nodes.size() == 1:
		var att = sword_nodes[0]
		_check("sword is a BoneAttachment3D on socket_main_hand (rides the Dolmen hand)",
			att is BoneAttachment3D and String(att.bone_name) == "socket_main_hand"
			and att.get_parent() == skel)
		_check("sword attachment carries a weapon mesh child",
			not att.find_children("*", "MeshInstance3D", true, false).is_empty())

	# Hide union (race-agnostic): the kit hides head, torso, hands, hips_legs, feet,
	# leaving forearms, lower_legs and waist as uncovered Dolmen body skin — the
	# SAME three regions Ardent shows. Proves the hides bind identically across races.
	var hidden_regions: Array = ["head", "torso", "hands", "hips_legs", "feet"]
	var visible_regions: Array = ["forearms", "lower_legs", "waist"]
	var hides_ok: bool = true
	for region in hidden_regions:
		var g: MeshInstance3D = ac.geoset_node(region)
		if g == null or g.visible:
			hides_ok = false
	_check("armour-covered Dolmen body regions are hidden (head/torso/hands/hips_legs/feet)", hides_ok)
	var uncovered_ok: bool = true
	for region in visible_regions:
		var g2: MeshInstance3D = ac.geoset_node(region)
		if g2 == null or not g2.visible:
			uncovered_ok = false
	_check("uncovered Dolmen regions stay visible (forearms/lower_legs/waist)", uncovered_ok)
	_check("exactly 3 Dolmen body geosets visible under the kit", _visible_geoset_count(ac) == 3)

	# Whole-composite render check: 3 body geosets + 6 armour + hair + sword = 11
	# visible meshes, matching the lead's D4 GPU render (11 visible LOD0 meshes).
	_check("exactly 11 visible meshes render on the assembled Dolmen (3 body + 6 armour + hair + sword)",
		_visible_mesh_total(ac) == 11)

	# Tear the kit + sword back down so later phases start clean.
	for slot in armor_slots:
		ac.set_equipment_slot(slot, 0, [])
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
# Uses Sylvane (race id 3): a frozen-but-unimplemented roster race with NO staged
# catalog. Dolmen (id 2) USED to be the catalog-miss case, but D3 (#625) added the
# Dolmen catalog — so this asserts against the next race that genuinely has none.
func _verify_catalog_miss(ac) -> void:
	print(" catalog miss — race with no catalog (Sylvane, id 3):")
	var ok: bool = ac.assemble(3, 0, {}, [])
	_check("assemble returns false", not ok and not ac.is_assembled())
	_check("assembly_failed names the missing catalog", _failures.has("catalog:3|0"))


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


# --- C. chibi-shape pack: pack roster + body_material body-dye (#760) -----------
# The C7 client proof, headless. A theme pack that ships a 6-colour-race roster + the
# appearance body_material recolor (design §8/§6), reusing the real staged core body +
# masks + russet dye so the dye path loads real bytes. Proves: (1) the PACK ROSTER drives
# the effective roster (6 races × 4 classes, beyond the compiled 4); (2) every colour race
# resolves a catalog carrying body_material (no catalog: errors); (3) assembling a colour
# race dyes the BODY — every geoset gets the dye_tint.gdshader ShaderMaterial with the
# race colour + localized metalness, albedo/mask bound — with ZERO assembly_failed.
func _verify_chibi_body_material(ac, db) -> void:
	print(" C. chibi-shape pack — pack roster + body_material body-dye (#760):")
	_write_chibi_fixture()
	var loaded: bool = db.load_from(CHIBI_FIXTURE_DIR)
	_check("chibi-shape fixture pack loads", loaded)

	# 1. Pack roster drives the pickers (design §8/R3) — 6 colour races × 4 classes.
	var races: Array = db.races()
	var classes: Array = db.classes()
	_check("pack ships 6 colour races", races.size() == 6)
	_check("races are ordered by roster_id 1..6",
		races.size() == 6 and int(races[0]["id"]) == 1 and int(races[5]["id"]) == 6)
	_check("race roster carries the colour names (Red..Silver)",
		races.size() == 6 and String(races[0]["name"]) == "Red"
		and String(races[4]["name"]) == "Gold" and String(races[5]["name"]) == "Silver")
	_check("pack ships 4 classes (Warrior/Mage/Rogue/Priest)", classes.size() == 4)
	_check("effective_races is the PACK roster (6, not the compiled 4)",
		db.effective_races().size() == 6)
	_check("pack roster validates colour races 5 & 6 (beyond the compiled 1..4)",
		db.is_valid_race(5) and db.is_valid_race(6) and not db.is_valid_race(7))

	# 2. Every colour race resolves a catalog carrying body_material (no catalog: errors).
	var all_catalogs := true
	var all_have_bm := true
	for rid in range(1, 7):
		var cat: Dictionary = db.catalog(rid, 0)
		if cat.is_empty():
			all_catalogs = false
		elif not cat.has("body_material"):
			all_have_bm = false
	_check("all 6 colour races resolve a catalog (pack race-name key mapping)", all_catalogs)
	_check("all 6 catalogs carry a body_material recolor", all_have_bm)

	# 3. Assemble race 5 (Gold — the METALLIC race) and prove the body carries the dye.
	var failures_before: int = _failures.size()
	var ok: bool = ac.assemble(5, 0, {}, [])
	_check("assemble(5,0) — Gold chibi assembles a real body (no capsule fallback)",
		ok and ac.is_assembled())
	_check("ZERO assembly_failed across the chibi body assemble",
		_failures.size() == failures_before)

	var russet: Color = db.dye_color(db.numeric_id_for("core:dye.russet"))  # fixture's gold tint
	var geosets: Array = ac.body_geosets()
	_check("chibi body instanced its geoset meshes", not geosets.is_empty())
	var every_geoset_dyed := not geosets.is_empty()
	var sample: ShaderMaterial = null
	for g in geosets:
		if not (g is MeshInstance3D):
			continue
		var sm := g.material_override as ShaderMaterial
		if sm == null or sm.shader == null \
				or sm.shader.resource_path != "res://characters/dye_tint.gdshader":
			every_geoset_dyed = false
			continue
		if sample == null:
			sample = sm
	_check("EVERY body geoset carries the dye_tint mask ShaderMaterial", every_geoset_dyed)
	if sample != null:
		var primary = sample.get_shader_parameter("dye_primary")
		_check("body dye_primary == the race dye colour (skin recolor)",
			primary is Color and (primary as Color).is_equal_approx(russet))
		# Unset use flags read back as null (the assembler leaves secondary/accent at the
		# shader's 0.0 default, exactly like the equipment path) — treat null as inactive.
		_check("primary channel active (use_primary == 1), others inactive",
			_flag_active(sample, "use_primary")
			and not _flag_active(sample, "use_secondary")
			and not _flag_active(sample, "use_accent"))
		_check("Gold race is metallic (metallic == 1.0 — shader localizes it to the skin mask)",
			float(sample.get_shader_parameter("metallic")) == 1.0)
		_check("the skin dye mask is bound", sample.get_shader_parameter("dye_mask") is Texture2D)
		_check("the neutral recolor-base albedo is bound",
			sample.get_shader_parameter("albedo_tex") is Texture2D)

	# 4. A FLAT colour race (Red, roster_id 1) assembles too, non-metallic body dye.
	var flat_before: int = _failures.size()
	var ok2: bool = ac.assemble(1, 0, {}, [])
	_check("assemble(1,0) — Red (flat) chibi assembles with ZERO assembly_failed",
		ok2 and ac.is_assembled() and _failures.size() == flat_before)
	var red_geosets: Array = ac.body_geosets()
	var red_sm: ShaderMaterial = null
	for g in red_geosets:
		if g is MeshInstance3D and g.material_override is ShaderMaterial:
			red_sm = g.material_override
			break
	_check("Red body is dyed but NOT metallic (metallic == 0.0)",
		red_sm != null and float(red_sm.get_shader_parameter("metallic")) == 0.0)

	ac.clear()


# A dye-shader use flag reads active (== 1.0) — null/unset counts as the 0.0 default.
func _flag_active(sm: ShaderMaterial, name: String) -> bool:
	var v = sm.get_shader_parameter(name)
	return v != null and float(v) == 1.0


# Write the chibi-shape fixture pack: a 6-colour-race roster + 4 classes + per-race
# appearance body_material, all reusing the real staged core body + masks + russet dye so
# the body-dye path loads real bytes (same reuse pattern as _write_fixture_pack). Red/Green
# flat (metallic 0); Gold/Silver metallic (1.0) — mirrors the real chibi pack's colour model.
func _write_chibi_fixture() -> void:
	DirAccess.make_dir_recursive_absolute(CHIBI_FIXTURE_DIR)

	# Only the ids the body-dye path touches need contents entries (body model + the two
	# masks reused as recolor-base/mask + the russet dye). Resources point at the real
	# staged core layout so model_path resolves to loadable staged .glb/.png bytes.
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

	# The 6 colour races (roster_id 1..6) — Gold(5)/Silver(6) metallic, the rest flat.
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
			# Reuse a real staged mask as the neutral recolor base + the skin mask so the
			# textures load; the assertion is on the SHADER params the assembler binds.
			"albedo": "core:art.item.armor.warden_head_mask",
			"dye_mask": "core:art.item.armor.warden_chest_mask",
			"metallic": metallic,
			"dyes": [{"channel": "primary", "dye": "core:dye.russet"}],
		}
		if metallic > 0.0:
			bm["roughness"] = 0.30
		appearance_rows.append({
			"id": "chibi:appearance.%s.male" % rname, "numeric_id": 1100 + rid,
			"race": rname, "sex": "male",
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
		"schema": "meridian/pack-data@1",
		"namespace": "chibi",
		"appearance": appearance_rows,
		"class": class_rows,
		"dye": [{"id": "core:dye.russet", "numeric_id": 78, "color": "#8a4b2d"}],
		"item": [],
		"race": race_rows,
	}
	var json := FileAccess.open(CHIBI_FIXTURE_DIR + "/pack.data.json", FileAccess.WRITE)
	json.store_string(JSON.stringify(data, "  "))
	json.close()


# How many BODY geoset meshes are currently visible (LOD0-only at M1).
func _visible_geoset_count(ac) -> int:
	var n: int = 0
	for g in ac.body_geosets():
		if g is MeshInstance3D and g.visible:
			n += 1
	return n


# Total MeshInstance3Ds that actually render on the whole assembled character —
# body geosets + equipped gear + hair + weapons — counting only those visible in
# the tree (hidden LOD1-3 and armour-covered geosets do not draw). The holistic
# "what the GPU render shows" count.
func _visible_mesh_total(ac) -> int:
	var n: int = 0
	for m in ac.find_children("*", "MeshInstance3D", true, false):
		if m.is_visible_in_tree():
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
