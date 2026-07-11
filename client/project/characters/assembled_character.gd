# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — AssembledCharacter: per-race body + worn gear assembly
# (story #540, client-assembler spec ② §4). The visual composition node the
# world scene and the char-select preview instantiate (Task 4): given the ids
# the wire carries (race/sex, appearance presets, equipment item_templates +
# dyes), it builds the visible character from mounted pack content via
# MeridianContentDB — body scene, single canonical Skeleton3D, skinned gear
# bound by bone name, weapons on socket_* BoneAttachment3Ds, geoset hides,
# M1 whole-piece dye tints.
#
# MODEL BYTES (M0 boundary, honest): ContentDB.model_path() returns the
# DECLARED imported-resource path (.scn) from pack.contents.jsonl — emit-pck is
# declarative at M0, no Godot importer has run, so those bytes do not exist yet.
# When the declared resource exists it is preferred (the future importer flow);
# until then the loader falls back to the SOURCE .glb staged next to the
# declared path (same by-ID layout, source extension — scripts/check-golden.sh
# stages and drift-gates those copies) and imports it at runtime through
# GLTFDocument/GLTFState. The .glb is a source asset, never a Godot resource.
#
# ERROR DISCIPLINE (spec §6, contract ① §9 — implemented verbatim):
#   * catalog miss (no race/sex entry)  → assemble() returns false; the caller
#     keeps its class-colored capsule fallback.
#   * unknown preset id                 → catalog entry 1 + assembly_failed.
#   * missing worn model                → the piece stays hidden + assembly_failed.
#   * unknown dye id                    → the item's authored colors (no tint).
# `assembly_failed` is emitted ONCE per failing asset id for this instance's
# lifetime (spec §6's "one telemetry warning per asset id per session").
#
# PRODUCED API (BINDING for Task 4 — spec ② Task 3):
#   assemble(race, sex, appearance, equipment) -> bool   # false ⇒ capsule fallback
#   set_equipment_slot(slot, item_template, dyes) -> void # incremental re-equip
#   clear() -> void
#   signal assembly_failed(reason: String)
#
# `appearance` carries preset ids: {hair: int, face: int, skin: int} (absent →
# 1). `equipment` is an Array of {slot: int, item_template: int, dyes: Array}.
# `dyes` is an Array of IF-9 dye numeric ids.

extends Node3D
class_name AssembledCharacter

## Emitted once per failing asset id (see ERROR DISCIPLINE above). `reason` is a
## stable "<kind>:<id>" string (e.g. "model:core:art.nope", "preset:hair:99").
signal assembly_failed(reason: String)

const _GEOSET_PREFIX: String = "geo_"
const _SOCKET_BONE_PREFIX: String = "socket_"

var _race: int = 0
var _sex: int = 0
var _assembled: bool = false
var _body_root: Node3D = null
var _skeleton: Skeleton3D = null
# The BODY's geoset MeshInstance3Ds, recorded at assemble() time so worn-gear
# meshes re-parented onto the skeleton later can never be hidden as geosets.
var _body_geosets: Array = []
# kind ("hair"/"face"/"skin") -> the RESOLVED catalog preset entry {id, model}.
var _applied_presets: Dictionary = {}
# slot -> {item_template: int, nodes: Array[Node3D], hides: Array[String]}.
var _slots: Dictionary = {}
# reason -> true; the once-per-failing-asset-id guard for assembly_failed.
var _failed: Dictionary = {}


# --- The BINDING API (spec ② Tasks 3/4) ---------------------------------------

## Build the character from wire ids. Returns false when no body could be
## assembled (catalog miss / unloadable body model) — the caller keeps its
## capsule fallback (spec §6). Re-entrant: always starts from clear().
func assemble(race: int, sex: int, appearance: Dictionary, equipment: Array) -> bool:
	clear()
	var db = MeridianContentDB.instance()
	var cat: Dictionary = db.catalog(race, sex)
	if cat.is_empty():
		_fail("catalog:%d|%d" % [race, sex])
		return false
	_race = race
	_sex = sex

	# Resolve appearance presets first (unknown id → entry 1 + assembly_failed).
	var presets: Dictionary = cat.get("presets", {"hair": [], "face": [], "skin": []})
	_applied_presets = {
		"hair": _resolve_preset(presets, "hair", int(appearance.get("hair", 1))),
		"face": _resolve_preset(presets, "face", int(appearance.get("face", 1))),
		"skin": _resolve_preset(presets, "skin", int(appearance.get("skin", 1))),
	}

	# The body: the catalog's body_model, one scene, one canonical Skeleton3D.
	var body_id: String = String(cat.get("body_model", ""))
	var body: Node3D = _load_model_scene(body_id)
	if body == null:
		return false  # _load_model_scene emitted the failure → capsule fallback
	body.name = "body"
	add_child(body)
	_body_root = body
	var skeletons: Array = body.find_children("*", "Skeleton3D", true, false)
	if skeletons.is_empty():
		_fail("skeleton:" + body_id)
		clear()
		return false
	_skeleton = skeletons[0]
	_body_geosets = body.find_children(_GEOSET_PREFIX + "*", "MeshInstance3D", true, false)
	_assembled = true

	# Hair preset = mesh on the head (spec §4). The blockout catalog aliases every
	# preset to the body model, so only a DISTINCT hair model mounts — the
	# machinery lands now, the real meshes with spec ⑤ content.
	var hair: Dictionary = _applied_presets.get("hair", {})
	var hair_model: String = String(hair.get("model", ""))
	if not hair_model.is_empty() and hair_model != body_id:
		_mount_skinned(hair_model)
	# Face/skin presets = material params (spec §4) — the blockout ships no
	# textures, so the resolved choice is recorded (applied_preset()) and the
	# material hookup arrives with real content in ⑤.

	for entry in equipment:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		set_equipment_slot(int(entry.get("slot", 0)), int(entry.get("item_template", 0)),
			entry.get("dyes", []))
	return true


## Equip (or, with item_template <= 0, unequip) one slot incrementally.
## Idempotent: re-equipping the same item replaces the piece wholesale.
func set_equipment_slot(slot: int, item_template: int, dyes: Array) -> void:
	if not _assembled:
		return
	_unequip(slot)
	if item_template <= 0:
		_apply_hides()
		return
	var db = MeridianContentDB.instance()
	var w: Dictionary = db.worn(item_template)
	if w.is_empty():
		# Unknown item / no worn block → the piece stays hidden (spec §6).
		_fail("worn:%d" % item_template)
		_apply_hides()
		return

	# race_overrides (item@2): an override for THIS race substitutes the FULL
	# {models, hides} shape wholesale (spec §4) — keys are roster race names.
	var models: Array = w.get("models", [])
	var hides: Array = w.get("hides", [])
	var overrides: Dictionary = w.get("race_overrides", {})
	var race_key: String = MeridianRoster.race_name(_race).to_lower()
	if overrides.has(race_key) and typeof(overrides[race_key]) == TYPE_DICTIONARY:
		var override: Dictionary = overrides[race_key]
		models = override.get("models", [])
		hides = override.get("hides", [])

	var attach: Dictionary = w.get("attach", {})
	var socket: String = String(attach.get("socket", ""))
	var nodes: Array = []
	for m in models:
		if typeof(m) != TYPE_DICTIONARY:
			continue
		var model_id: String = String(m.get("model", ""))
		var mirror: String = String(m.get("mirror", "none"))
		if socket.is_empty():
			nodes.append_array(_mount_skinned(model_id))
		else:
			var mounted: Node3D = _mount_socketed(model_id, socket, mirror)
			if mounted != null:
				nodes.append(mounted)
		# A model that failed to mount already emitted assembly_failed — the
		# piece (or that part of it) simply stays hidden (spec §6).

	_apply_dyes(nodes, dyes)
	_slots[slot] = {"item_template": item_template, "nodes": nodes, "hides": hides.duplicate()}
	_apply_hides()


## Tear down everything assemble() and set_equipment_slot() built. The
## once-per-asset-id assembly_failed guard intentionally survives (per-session
## telemetry discipline, spec §6).
func clear() -> void:
	for slot in _slots.keys():
		_unequip(slot)
	_slots.clear()
	if _body_root != null and is_instance_valid(_body_root):
		_body_root.free()
	_body_root = null
	_skeleton = null
	_body_geosets = []
	_applied_presets = {}
	_assembled = false
	_race = 0
	_sex = 0


# --- Read-side helpers (Task 4 diagnostics + headless verify) -----------------

## True once assemble() has built a body (and until clear()).
func is_assembled() -> bool:
	return _assembled


## The single canonical Skeleton3D of the assembled body (null when unassembled).
func body_skeleton() -> Skeleton3D:
	return _skeleton


## The RESOLVED catalog preset entry {id, model} applied for `kind`
## ("hair"/"face"/"skin") — after an unknown-id fallback this reports entry 1.
func applied_preset(kind: String) -> Dictionary:
	return _applied_presets.get(kind, {})


## The piece nodes mounted for `slot` ([] when empty / piece hidden).
func equipped_nodes(slot: int) -> Array:
	var rec: Dictionary = _slots.get(slot, {})
	var nodes: Array = rec.get("nodes", [])
	return nodes


## The body geoset MeshInstance3D for a region name (e.g. "feet" →
## geo_feet_lod0), or null. Only BODY geosets — never worn-gear meshes.
func geoset_node(region: String) -> MeshInstance3D:
	for g in _body_geosets:
		if is_instance_valid(g) and _region_of(String(g.name)) == region:
			return g
	return null


# --- Internals -----------------------------------------------------------------

# Resolve one appearance preset: the catalog entry whose id matches `wanted`,
# else catalog entry 1 + assembly_failed (spec §6 "unknown preset id → catalog
# entry 1 + telemetry warning"). Returns {} when the catalog has no entries for
# this kind at all.
func _resolve_preset(presets: Dictionary, kind: String, wanted: int) -> Dictionary:
	var entries: Array = presets.get(kind, [])
	var fallback: Dictionary = {}
	for e in entries:
		if typeof(e) != TYPE_DICTIONARY:
			continue
		if int(e.get("id", 0)) == wanted:
			return e
		if fallback.is_empty() and int(e.get("id", 0)) == 1:
			fallback = e
	_fail("preset:%s:%d" % [kind, wanted])
	if fallback.is_empty() and not entries.is_empty() and typeof(entries[0]) == TYPE_DICTIONARY:
		fallback = entries[0]
	return fallback


# Load a model by content id via ContentDB: prefer the DECLARED imported
# resource when it exists, else runtime-import the staged source .glb sibling
# (see MODEL BYTES header). Returns null (with assembly_failed) on any miss.
func _load_model_scene(model_id: String) -> Node3D:
	if model_id.is_empty():
		_fail("model:<empty>")
		return null
	var db = MeridianContentDB.instance()
	var declared: String = db.model_path(model_id)
	if declared.is_empty():
		_fail("model:" + model_id)
		return null
	if ResourceLoader.exists(declared):
		var packed = load(declared)
		if packed is PackedScene:
			var inst: Node = packed.instantiate()
			if inst is Node3D:
				inst.set_meta("model_id", model_id)
				return inst
			inst.free()
		_fail("model:" + model_id)
		return null
	var glb_path: String = declared.get_basename() + ".glb"
	if not FileAccess.file_exists(glb_path):
		_fail("model:" + model_id)
		return null
	var bytes: PackedByteArray = FileAccess.get_file_as_bytes(glb_path)
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err: int = doc.append_from_buffer(bytes, glb_path.get_base_dir(), state)
	if err != OK:
		_fail("model:" + model_id)
		return null
	var scene: Node = doc.generate_scene(state)
	if scene == null:
		_fail("model:" + model_id)
		return null
	if not (scene is Node3D):
		scene.free()
		_fail("model:" + model_id)
		return null
	scene.set_meta("model_id", model_id)
	return scene


# Skinned gear: re-parent the model's MeshInstance3Ds onto the body skeleton.
# Binding is BY BONE NAME (the contract): each mesh's Skin resource carries
# canonical bone names that must resolve against the body skeleton. Returns the
# re-parented meshes ([] + assembly_failed when the model cannot be mounted).
func _mount_skinned(model_id: String) -> Array:
	var scene: Node3D = _load_model_scene(model_id)
	if scene == null:
		return []
	var out: Array = []
	var meshes: Array = scene.find_children("*", "MeshInstance3D", true, false)
	for mi in meshes:
		mi.owner = null  # leaving the generated scene — owner would be stale
		var parent: Node = mi.get_parent()
		if parent != null:
			parent.remove_child(mi)
		_skeleton.add_child(mi)
		mi.skeleton = NodePath("..")
		mi.set_meta("model_id", model_id)
		if mi.skin != null and not _skin_binds_resolve(mi.skin):
			_fail("skin:" + model_id)
		out.append(mi)
	scene.free()
	if out.is_empty():
		_fail("model-empty:" + model_id)
	return out


# Weapons/props: a BoneAttachment3D on the socket_* bone named by
# worn.attach.socket, with the model scene as its child. mirror == "x" flips
# the mounted scene (M1: socketed pieces only — skinned mirroring needs real
# mirrored meshes and arrives with spec ⑤ content).
func _mount_socketed(model_id: String, socket: String, mirror: String) -> Node3D:
	var bone: String = _SOCKET_BONE_PREFIX + socket
	if _skeleton.find_bone(bone) < 0:
		_fail("socket:" + bone)
		return null
	var scene: Node3D = _load_model_scene(model_id)
	if scene == null:
		return null
	if mirror == "x":
		scene.scale = Vector3(-1.0, 1.0, 1.0)
	var attachment := BoneAttachment3D.new()
	attachment.name = "attach_" + socket
	_skeleton.add_child(attachment)
	attachment.bone_name = bone
	attachment.add_child(scene)
	attachment.set_meta("model_id", model_id)
	return attachment


# Every Skin bind name must resolve to a bone on the body skeleton (bind-by-
# index entries have no name and pass — the canonical rig binds by name).
func _skin_binds_resolve(skin: Skin) -> bool:
	for i in range(skin.get_bind_count()):
		var bind_name: String = String(skin.get_bind_name(i))
		if bind_name.is_empty():
			continue
		if _skeleton.find_bone(bind_name) < 0:
			return false
	return true


func _unequip(slot: int) -> void:
	if not _slots.has(slot):
		return
	var rec: Dictionary = _slots[slot]
	for n in rec.get("nodes", []):
		if is_instance_valid(n):
			n.free()  # free() detaches from the parent skeleton too
	_slots.erase(slot)


# Geoset hides = the UNION of every equipped slot's hides (spec §4). Recomputing
# the union on every change IS the per-slot restore tracking: a region stays
# hidden while any equipped item hides it and reappears when the last one goes.
func _apply_hides() -> void:
	var hidden: Dictionary = {}
	for slot in _slots:
		for region in _slots[slot].get("hides", []):
			hidden[String(region)] = true
	for g in _body_geosets:
		if is_instance_valid(g):
			g.visible = not hidden.has(_region_of(String(g.name)))


# "geo_<region>_lod<n>" -> "<region>" (tolerates any lod suffix / none).
func _region_of(node_name: String) -> String:
	var s: String = node_name.trim_prefix(_GEOSET_PREFIX)
	var lod_at: int = s.rfind("_lod")
	if lod_at >= 0:
		s = s.substr(0, lod_at)
	return s


# M1 dye: whole-piece albedo tint (spec §4) — the first KNOWN dye id tints every
# surface of the piece via a material override; unknown ids keep the item's
# authored colors (spec §6). The per-channel dye-mask path arrives with real
# textures (spec ⑤).
func _apply_dyes(nodes: Array, dyes: Array) -> void:
	if dyes.is_empty() or nodes.is_empty():
		return
	var db = MeridianContentDB.instance()
	var tint: Color = MeridianContentDB.UNKNOWN_DYE
	for d in dyes:
		var c: Color = db.dye_color(int(d))
		if c != MeridianContentDB.UNKNOWN_DYE:
			tint = c
			break
		_fail("dye:%d" % int(d))
	if tint == MeridianContentDB.UNKNOWN_DYE:
		return
	var mat := StandardMaterial3D.new()
	mat.albedo_color = tint
	for n in nodes:
		if n is MeshInstance3D:
			n.material_override = mat
		elif n is Node:
			for mi in n.find_children("*", "MeshInstance3D", true, false):
				mi.material_override = mat


# Emit assembly_failed ONCE per failing asset id (instance lifetime) + a
# telemetry warning (spec §6).
func _fail(reason: String) -> void:
	if _failed.has(reason):
		return
	_failed[reason] = true
	push_warning("AssembledCharacter: assembly failure — %s" % reason)
	assembly_failed.emit(reason)
