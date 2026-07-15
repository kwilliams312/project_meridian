# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — AssembledCharacter: per-race body + worn gear assembly
# (story #540, client-assembler spec ② §4). The visual composition node the
# world scene and the char-select preview instantiate (Task 4): given the ids
# the wire carries (race/sex, appearance presets, equipment item_templates +
# dyes), it builds the visible character from mounted pack content via
# MeridianContentDB — body scene, single canonical Skeleton3D, skinned gear
# bound by bone name, weapons on socket_* BoneAttachment3Ds, geoset hides,
# and per-channel mask-tint dyes (⑤/S3 — a dye-mask shader replaces the M1
# whole-piece albedo tint).
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
# `dyes` is an Array of {channel: int, dye_id: int} (⑤/S3, #570 — the codec/pump
# carry the dye `channel`: 0=primary, 1=secondary, 2=accent, selecting which RGB
# region of the piece's dye mask the numeric dye_id tints).

extends Node3D
class_name AssembledCharacter

## Emitted once per failing asset id (see ERROR DISCIPLINE above). `reason` is a
## stable "<kind>:<id>" string (e.g. "model:core:art.nope", "preset:hair:99").
signal assembly_failed(reason: String)

# Preload-by-path (not the bare `MeridianContentDB` class name): a freshly-added
# global class_name can be unresolvable against a stale .godot class cache — the
# exact trap T2's fix round banned (see content_db_verify.gd, char_select.gd:46).
const ContentDbScript := preload("res://content/content_db.gd")

# The dye mask-tint shader (⑤/S3): ONE parameterized variant of the Character
# master material (Art PRD §2.3 — not a new master). Every dyed piece gets a
# ShaderMaterial off this shader with its mask + per-channel colours.
const DyeShader := preload("res://characters/dye_tint.gdshader")

# A dyeable piece's dye mask asset id is its model content id + this suffix
# (e.g. core:art.item.armor.warden_chest → ..._mask), staged as an RGB PNG.
const _DYE_MASK_SUFFIX: String = "_mask"

const _GEOSET_PREFIX: String = "geo_"
const _SOCKET_BONE_PREFIX: String = "socket_"

# Body-material dye channel NAMES -> the RGB mask index the dye shader multiplies
# (mirrors the wire dye `channel` ints 0/1/2 — ⑤/S3). The chibi appearance
# body_material declares its skin dye on the "primary" channel (design §6/R2).
const _DYE_CHANNEL_INDEX: Dictionary = {"primary": 0, "secondary": 1, "accent": 2}

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
	var db = ContentDbScript.instance()
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
	# Establish the LOD0-only visibility invariant immediately (before any gear):
	# a real body ships an authored LOD0-3 chain, and all levels import VISIBLE —
	# without this a body-only assemble would draw all 4 LODs stacked. Equipment
	# changes re-run _apply_hides, but a no-equipment assemble needs it here.
	_apply_hides()

	# Body-material recolor (chibi colour races, design §6/R2): when the catalog
	# declares a body_material, dye the BODY skin with the race's colour through the
	# skin mask — the SAME mask-tint path equipment uses (⑤/S3), extended from
	# equipment-only to the body. A catalog without body_material (the core races,
	# which bake colour into the body model) skips this entirely — no visual change.
	_apply_body_material(cat)

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
	var db = ContentDbScript.instance()
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

	_apply_dyes(nodes, dyes, w)
	# Lead ruling (②/T4, #541): when an item DECLARES models but NONE of them mounted
	# (every model failed to load), do NOT record its geoset hides — hiding a body region
	# while the covering piece is invisible would leave that region uncovered ("hide-while-
	# uncovered"). A pure-hide item (no models authored) still hides as authored. This
	# supersedes the spec-literal "missing worn model → hide the piece + log": we keep the
	# piece hidden (no nodes) but stop it from also stripping the body underneath.
	var effective_hides: Array = hides
	if not models.is_empty() and nodes.is_empty():
		effective_hides = []
	_slots[slot] = {"item_template": item_template, "nodes": nodes,
		"hides": effective_hides.duplicate()}
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
## geo_feet_lod0), or null. Only BODY geosets — never worn-gear meshes. Prefers
## the LOD0 mesh (the one that renders at M1); falls back to any LOD of the
## region if no LOD0 exists.
func geoset_node(region: String) -> MeshInstance3D:
	var fallback: MeshInstance3D = null
	for g in _body_geosets:
		if is_instance_valid(g) and _region_of(String(g.name)) == region:
			if _is_lod0(String(g.name)):
				return g
			if fallback == null:
				fallback = g
	return fallback


## Every BODY geoset MeshInstance3D across all LOD levels (worn gear is never
## here). Diagnostics / headless verify only — visibility is owned by
## _apply_hides (LOD0 only; see there).
func body_geosets() -> Array:
	return _body_geosets


# --- Internals -----------------------------------------------------------------

# Resolve one appearance preset: the catalog entry whose id matches `wanted`,
# else catalog entry 1 + assembly_failed (spec §6 "unknown preset id → catalog
# entry 1 + telemetry warning"). Returns {} when the catalog has no entries for
# this kind at all.
func _resolve_preset(presets: Dictionary, kind: String, wanted: int) -> Dictionary:
	var entries: Array = presets.get(kind, [])
	# An EMPTY preset list is not a miss — the catalog simply offers no customization on
	# this channel (a chibi colour race ships empty hair/face/skin presets for now, design
	# §5/§8). Resolve to {} WITHOUT assembly_failed; only a non-empty list with the wanted
	# id absent is the spec-§6 "unknown preset id → entry 1 + telemetry" case below.
	if entries.is_empty():
		return {}
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
	var db = ContentDbScript.instance()
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
			var region: String = _region_of(String(g.name))
			# Only LOD0 geosets render at M1; the authored lod1-3 meshes are kept
			# hidden for the DEFERRED distance-based LOD system (⑤ spec §8 / Art
			# PRD §2.5 crowd perf). Drawing them would stack every authored LOD
			# level on each character and blow the 50-player crowd budget. A LOD0
			# geoset is visible unless an equipped piece's worn.hides covers its
			# region — and because the hide test is region-keyed, hiding a region
			# hides ALL its LODs, not just LOD0.
			g.visible = _is_lod0(String(g.name)) and not hidden.has(region)


# Whether a geoset mesh name is the base LOD that renders at M1. "geo_torso_lod0"
# → true; "geo_torso_lod1".."lod3" → false; a name with no "_lod" suffix (the old
# single-level blockout convention) counts as base/renderable.
func _is_lod0(node_name: String) -> bool:
	var lod_at: int = node_name.rfind("_lod")
	if lod_at < 0:
		return true
	return node_name.substr(lod_at + 4) == "0"


# "geo_<region>_lod<n>" -> "<region>" (tolerates any lod suffix / none).
func _region_of(node_name: String) -> String:
	var s: String = node_name.trim_prefix(_GEOSET_PREFIX)
	var lod_at: int = s.rfind("_lod")
	if lod_at >= 0:
		s = s.substr(0, lod_at)
	return s


# Mask-tint dye (⑤/S3, spec §4 / ① §6): each dyed piece gets a ShaderMaterial
# (DyeShader) that samples the piece's RGB dye mask and multiplies each channel's
# region (R=primary, G=secondary, B=accent) by that channel's chosen dye colour,
# preserving unmasked albedo. `dyes` is the wire shape [{channel:int, dye_id:int}]
# (the codec/pump carry the channel — ⑤/S3). Fallbacks (spec §6 / ① §9):
#   * a piece with no dye mask OR no worn.dye_channels → no tint (authored colours);
#   * an unknown dye id → that channel stays untinted (+ assembly_failed once);
#   * every dye unknown → no material override at all (piece keeps authored colours).
func _apply_dyes(nodes: Array, dyes: Array, worn: Dictionary) -> void:
	if dyes.is_empty() or nodes.is_empty():
		return
	# A piece must DECLARE itself dyeable (worn.dye_channels) to tint at all.
	var channels: Array = worn.get("dye_channels", [])
	if channels.is_empty():
		return
	var db = ContentDbScript.instance()

	# Resolve the chosen dye colour per RGB channel index from the wire choices.
	# Only KNOWN dyes populate the map; unknown ids fall back to authored colours.
	var channel_colors: Dictionary = {}  # int channel (0/1/2) -> Color
	for d in dyes:
		if typeof(d) != TYPE_DICTIONARY:
			continue
		var ch: int = int(d.get("channel", 0))
		var dye_id: int = int(d.get("dye_id", 0))
		var c: Color = db.dye_color(dye_id)
		if c == ContentDbScript.UNKNOWN_DYE:
			_fail("dye:%d" % dye_id)
			continue
		channel_colors[ch] = c
	# No known dye resolved → keep the piece's authored colours (no override).
	if channel_colors.is_empty():
		return

	for n in nodes:
		if n is MeshInstance3D:
			_tint_mesh(n, channel_colors)
		elif n is Node:
			for mi in n.find_children("*", "MeshInstance3D", true, false):
				_tint_mesh(mi, channel_colors)


# Build and apply the mask-tint ShaderMaterial for ONE mesh. Resolves the mesh's
# dye mask from its model id (model_id + _mask); a mesh with no resolvable mask
# is left with its authored material (no tint). The authored albedo (colour +
# texture) is copied into the shader so unmasked regions stay pixel-identical.
func _tint_mesh(mi: MeshInstance3D, channel_colors: Dictionary) -> void:
	var model_id: String = String(mi.get_meta("model_id", ""))
	if model_id.is_empty():
		return
	var mask: Texture2D = _load_mask_texture(model_id + _DYE_MASK_SUFFIX)
	if mask == null:
		# The piece declares dye_channels but ships no mask → no tint (spec §4).
		return

	var sm := ShaderMaterial.new()
	sm.shader = DyeShader
	sm.set_shader_parameter("dye_mask", mask)

	# Preserve the authored albedo (material_override REPLACES the material).
	var base: Material = mi.get_active_material(0)
	var bm := base as BaseMaterial3D
	if bm != null:
		sm.set_shader_parameter("albedo_color", bm.albedo_color)
		if bm.albedo_texture != null:
			sm.set_shader_parameter("albedo_tex", bm.albedo_texture)
		sm.set_shader_parameter("roughness", bm.roughness)
		sm.set_shader_parameter("metallic", bm.metallic)

	# Bind each chosen channel colour + flip its use flag on (default off → the
	# authored albedo shows through even where that channel's mask is painted).
	if channel_colors.has(0):
		sm.set_shader_parameter("dye_primary", channel_colors[0])
		sm.set_shader_parameter("use_primary", 1.0)
	if channel_colors.has(1):
		sm.set_shader_parameter("dye_secondary", channel_colors[1])
		sm.set_shader_parameter("use_secondary", 1.0)
	if channel_colors.has(2):
		sm.set_shader_parameter("dye_accent", channel_colors[2])
		sm.set_shader_parameter("use_accent", 1.0)

	mi.material_override = sm


# Body-material recolor (chibi colour races, design 2026-07-14-chibi §6/R2). The
# catalog's `body_material` = {albedo, dye_mask, metallic, [roughness], dyes:[{channel,
# dye}]}: the client multiplies the neutral `albedo` recolor base by each dyed channel's
# colour through `dye_mask` — the SAME dye_tint.gdshader mask path equipment uses (⑤/S3),
# applied to the BODY geoset meshes. Only the mask's painted skin region recolors (R =
# skin on the chibi mask), so the atlas eyes + cloth wrap survive; a metallic race
# (Gold/Silver) sets metallic 1.0 and the shader localizes metalness to the same skin
# region (METALLIC = metallic * max(mask.rgb)). Fallbacks mirror the equipment path:
#   * no body_material                       → no-op (core races bake colour in the model);
#   * no resolvable albedo/mask texture      → skip (body keeps its imported material);
#   * an unknown dye id                      → that channel stays untinted (+ assembly_failed);
#   * every dye unknown                      → no override (body keeps its imported material).
func _apply_body_material(cat: Dictionary) -> void:
	var bm: Dictionary = cat.get("body_material", {})
	if bm.is_empty() or _body_geosets.is_empty():
		return

	# The neutral recolor base + the skin mask (both art-asset ids resolved as textures).
	var albedo_tex: Texture2D = _load_mask_texture(String(bm.get("albedo", "")))
	var mask: Texture2D = _load_mask_texture(String(bm.get("dye_mask", "")))
	if albedo_tex == null or mask == null:
		# Missing recolor bytes → leave the body's imported material (no crash, spec §6).
		return

	var db = ContentDbScript.instance()
	# Resolve each declared channel's dye colour (name -> RGB index -> Color). Only KNOWN
	# dyes populate the map; an unknown id stays untinted for that channel (authored base).
	var channel_colors: Dictionary = {}  # int channel (0/1/2) -> Color
	for d in bm.get("dyes", []):
		if typeof(d) != TYPE_DICTIONARY:
			continue
		var ch: int = int(_DYE_CHANNEL_INDEX.get(String(d.get("channel", "")), -1))
		if ch < 0:
			continue
		var dye_ref = d.get("dye", "")
		# body_material carries the dye as a content id String; resolve it to the numeric
		# the dye table is keyed by (the equipment path already receives numeric ids).
		var dye_num: int = int(dye_ref) if dye_ref is int else db.numeric_id_for(String(dye_ref))
		var c: Color = db.dye_color(dye_num)
		if c == ContentDbScript.UNKNOWN_DYE:
			_fail("dye:%s" % str(dye_ref))
			continue
		channel_colors[ch] = c
	# No known dye resolved → keep the body's imported material (no override).
	if channel_colors.is_empty():
		return

	# Every body geoset shares ONE material (same recolor base + mask + dyes), so build it
	# once and assign it to all of them — a Godot material can back multiple meshes.
	var sm := ShaderMaterial.new()
	sm.shader = DyeShader
	sm.set_shader_parameter("dye_mask", mask)
	# The recolor base IS the authored albedo the shader tints (albedo_color white so the
	# texture drives it, matching _tint_mesh's albedo-preservation contract).
	sm.set_shader_parameter("albedo_tex", albedo_tex)
	sm.set_shader_parameter("albedo_color", Color(1, 1, 1, 1))
	sm.set_shader_parameter("metallic", float(bm.get("metallic", 0.0)))
	if bm.has("roughness"):
		sm.set_shader_parameter("roughness", float(bm["roughness"]))
	if channel_colors.has(0):
		sm.set_shader_parameter("dye_primary", channel_colors[0])
		sm.set_shader_parameter("use_primary", 1.0)
	if channel_colors.has(1):
		sm.set_shader_parameter("dye_secondary", channel_colors[1])
		sm.set_shader_parameter("use_secondary", 1.0)
	if channel_colors.has(2):
		sm.set_shader_parameter("dye_accent", channel_colors[2])
		sm.set_shader_parameter("use_accent", 1.0)

	for g in _body_geosets:
		if g is MeshInstance3D:
			g.material_override = sm


# Load a dye mask texture by asset id: prefer the DECLARED imported resource when
# it exists, else runtime-load the staged source .png sibling (same MODEL BYTES
# fallback as _load_model_scene). Returns null when the mask cannot be resolved
# (→ the piece keeps its authored colours).
func _load_mask_texture(mask_id: String) -> Texture2D:
	if mask_id.is_empty():
		return null
	var db = ContentDbScript.instance()
	var declared: String = db.model_path(mask_id)
	if declared.is_empty():
		return null
	if ResourceLoader.exists(declared):
		var res = load(declared)
		if res is Texture2D:
			return res
		return null
	var png_path: String = declared.get_basename() + ".png"
	if not FileAccess.file_exists(png_path):
		return null
	var img := Image.new()
	var err: int = img.load(png_path)
	if err != OK:
		return null
	return ImageTexture.create_from_image(img)


# Emit assembly_failed ONCE per failing asset id (instance lifetime) + a
# telemetry warning (spec §6).
func _fail(reason: String) -> void:
	if _failed.has(reason):
		return
	_failed[reason] = true
	push_warning("AssembledCharacter: assembly failure — %s" % reason)
	assembly_failed.emit(reason)
