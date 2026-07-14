# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verify for the DEV chibi_pill_body preview
# (story #747, epic #722). NOT a shipped scene: a SceneTree script run the same way
# as characters/assembled_character_verify.gd —
#   godot --headless --path client/project --import   # once, seed the class cache
#   godot --headless --path client/project --script res://scenes/dev/chibi_body_preview_verify.gd
# — so CI / a dev box can prove, with no render and no server, that the landed
# `core:art.chibi_pill_body` base assembles + rigs correctly through the REAL engine
# load path (AssembledCharacter → ContentDB.model_path → the staged .glb), using the
# SAME throwaway dev catalog the visible preview scene uses (chibi_body_preview.gd).
#
# ⛔ Run `--import` FIRST. The assembler + the dev catalog resolve the MeridianRoster
# global class, which lives only in .godot/global_script_class_cache.cfg — a fresh
# checkout has no cache, so `--script` alone would fail to find the class. A single
# headless `--import` pass builds the cache (and imports the staged art).
#
# Asserts the story #747 DoD: is_assembled() == true (the real body, NOT the capsule
# fallback), a single canonical Skeleton3D with the 63 canonical bones, the 8 LOD0
# geosets visible, and ZERO assembly_failed across the whole assemble.
#
# Exits 0 on success, 1 on any failed assertion.

extends SceneTree

# Standalone --script mode never initializes autoloads — preload the scripts and use
# the shared MeridianContentDB instance() (content_db.gd ACCESS PATTERN).
const ContentDbScript := preload("res://content/content_db.gd")
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")
const ChibiPreviewCatalog := preload("res://scenes/dev/chibi_body_preview_catalog.gd")

# The 8 blockout/body geoset regions (tools/blender/meridian_rig generate_blockout.py;
# the chibi base is cut into the same 8 geo_<region>_lod<N> geosets).
const REGIONS: Array = [
	"feet", "forearms", "hands", "head", "hips_legs", "lower_legs", "torso", "waist",
]

var _fails: int = 0
var _failures: Array = []  # captured assembly_failed reasons, in emit order


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _on_assembly_failed(reason: String) -> void:
	_failures.append(reason)


func _initialize() -> void:
	print("meridian chibi_pill_body DEV preview RUNTIME verify (#747)")

	var db = ContentDbScript.instance()
	var dev_race: int = ChibiPreviewCatalog.install(db)
	_check("throwaway dev catalog loaded", db.is_loaded())
	_check("catalog(DEV_RACE, DEV_SEX) resolves the chibi body",
		String(db.catalog(dev_race, ChibiPreviewCatalog.DEV_SEX).get("body_model", ""))
			== ChibiPreviewCatalog.BODY_ID)
	_check("chibi model id resolves to a staged resource",
		not db.model_path(ChibiPreviewCatalog.BODY_ID).is_empty())

	var ac = AssembledCharacterScript.new()
	ac.name = "chibi_body_under_test"
	root.add_child(ac)
	ac.assembly_failed.connect(_on_assembly_failed)

	# Assemble the bare chibi body — no gear, empty appearance dict (presets default
	# to id 1, which the dev catalog aliases to the body model → no preset failures).
	var ok: bool = ac.assemble(dev_race, ChibiPreviewCatalog.DEV_SEX, {}, [])
	_check("assemble returns true", ok)
	_check("is_assembled() == true (real body, NOT the capsule fallback)", ac.is_assembled())

	var skel: Skeleton3D = ac.body_skeleton()
	_check("single Skeleton3D present", skel != null)
	_check("skeleton has the 63 canonical bones", skel != null and skel.get_bone_count() == 63)
	_check("socket_main_hand bone present (canonical rig)",
		skel != null and skel.find_bone("socket_main_hand") >= 0)

	# All 8 body geosets instanced + visible at LOD0 (bare body, no hides).
	var visible_count: int = 0
	for region in REGIONS:
		var g: MeshInstance3D = ac.geoset_node(region)
		if g != null and g.visible:
			visible_count += 1
	_check("all 8 body geosets instanced and visible (LOD0)", visible_count == 8)

	# THE CRUX (story #747): zero assembly_failed across the whole assemble.
	_check("ZERO assembly_failed across the chibi body assemble", _failures.is_empty())
	if not _failures.is_empty():
		print("  assembly_failed reasons: %s" % [_failures])

	root.remove_child(ac)
	ac.free()

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)
