# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — runtime loader for RETARGETED locomotion clips (story #914,
# W2a of the character-animation epic #906). This is the runtime half of "Path B"
# (offline pre-retarget -> runtime GLTF load); the offline half is the Blender
# tool tools/blender/meridian_rig/retarget_clip.py.
#
# WHAT IT DOES. Loads the staged animation .glb the Blender tool produced (an
# external humanoid clip baked onto the canonical SkeletonProfileHumanoid rig),
# pulls its idle/walk/run/jump clips, RE-KEYS every rotation track onto the LIVE
# runtime skeleton, and returns an AnimationLibrary under the SAME name +
# clip-keys the W1a placeholder uses (MeridianLocomotion.LIBRARY_NAME) — so the
# W1a AnimationTree drives the real clips UNCHANGED (it references
# "<library>/<clip>", transparent to which library provides them).
#
# WHY RE-KEY (the seam W1a established). GLTFDocument.generate_scene builds an
# AnimationPlayer whose bone tracks are keyed "<Armature>/Skeleton3D:<Bone>" —
# paths relative to the GLB's OWN generated scene. The AssembledCharacter's
# AnimationPlayer roots at the character node, so its tracks must read
# "<self→skeleton>:<Bone>". We rebuild each track path against the live skeleton,
# exactly the "<skel_path>:<bone>" convention MeridianLocomotion.build_library
# keys to — so both libraries are byte-for-byte interchangeable to the tree.
#
# REST REBASE (defense-in-depth). A ROTATION_3D key is the bone's FULL local
# rotation, i.e. rest ∘ motion. The retarget bakes onto a rest built from the
# SAME bones.py table the chibi rig ships from, so the GLB rest and the live rest
# are byte-identical in practice (verified: every sampled bone rest angle 0.0000)
# and the rebase reduces to identity. We still rebase — q_live = live_rest ∘
# glb_rest⁻¹ ∘ q_glb — so a future rig whose rest drifts from the table still
# plays correctly (the MOTION delta is preserved, re-anchored to the live rest)
# instead of silently mis-posing.
#
# ROTATION-ONLY / IN-PLACE. Only ROTATION_3D tracks are re-keyed; any
# position/scale tracks are dropped, keeping the character in place (root motion
# is the mover's job, same contract as the W1a placeholder). Tracks whose bone is
# absent on the live skeleton are skipped (the safe-fallback contract) — a rig
# missing a bone is a quiet no-op, never a load error.
#
# PROVENANCE. The staged fixture is authored ORIGINAL data (no third-party clip,
# no Mixamo/Adobe license); the library is stamped source_tier: original, no IF-8
# sidecar — same reasoning as the W1a placeholder (asset.schema.yaml has no
# animation-clip class yet; that + the real Mixamo license are the human-gated
# W3-prov story). Real Mixamo clips are Wave 3, NOT this story.
#
# FALLBACK. Any failure (missing glb, load error, absent player/skeleton, a clip
# missing) returns null so the caller (AssembledCharacter._build_locomotion) can
# fall back to the code-authored W1a placeholder library — locomotion never
# breaks just because the staged clip is unavailable.

extends RefCounted
class_name MeridianLocomotionClips

const LocomotionScript := preload("res://characters/locomotion.gd")

## Staged animation glb (produced by tools/blender/meridian_rig/retarget_clip.py).
## Lives in a .gdignore'd dir so Godot's editor importer never runs on it — the
## raw bytes are read at runtime via FileAccess + GLTFDocument, exactly like the
## chibi body glb (no committed .import artifact; the whole content pipeline is
## staged-source + runtime-load).
const GLB_PATH: String = "res://characters/anim/meridian_locomotion_retarget.glb"

## The clip keys the retargeted library must provide — the SAME idle/walk/run/jump
## keys the W1a AnimationTree references. Mirrors MeridianLocomotion.CLIP_*.
const CLIP_KEYS: Array = ["idle", "walk", "run", "jump"]


## Build the retargeted locomotion AnimationLibrary for `skeleton`. `skel_path`
## is the NodePath (from the owning AnimationPlayer's root_node) to the skeleton;
## it prefixes every re-keyed track path ("<skel_path>:<bone>"). Returns a library
## carrying the four clips under LocomotionScript.LIBRARY_NAME, stamped
## source_tier: original — or `null` on ANY failure (caller falls back to the W1a
## placeholder). `glb_path` is injectable for tests.
static func build_library_from_glb(skeleton: Skeleton3D, skel_path: NodePath,
		glb_path: String = GLB_PATH) -> AnimationLibrary:
	if skeleton == null:
		return null
	if not FileAccess.file_exists(glb_path):
		return null
	var bytes: PackedByteArray = FileAccess.get_file_as_bytes(glb_path)
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_buffer(bytes, glb_path.get_base_dir(), state) != OK:
		return null
	var scene: Node = doc.generate_scene(state)
	if scene == null:
		return null

	# The generated scene carries the GLB's own skeleton (rest source) + an
	# AnimationPlayer (clip source). It is never added to the tree — freed below.
	var skels: Array = scene.find_children("*", "Skeleton3D", true, false)
	var players: Array = scene.find_children("*", "AnimationPlayer", true, false)
	if skels.is_empty() or players.is_empty():
		scene.free()
		return null
	var glb_skel: Skeleton3D = skels[0]
	var glb_player: AnimationPlayer = players[0]

	# GLB-rest map: bone name -> rest rotation quaternion (for the rebase).
	var glb_rest: Dictionary = {}
	for i in range(glb_skel.get_bone_count()):
		glb_rest[glb_skel.get_bone_name(i)] = \
			glb_skel.get_bone_rest(i).basis.get_rotation_quaternion()

	var lib := AnimationLibrary.new()
	lib.resource_name = LocomotionScript.LIBRARY_NAME
	lib.set_meta("source_tier", LocomotionScript.SOURCE_TIER)
	# A marker the verify uses to prove the RETARGETED path loaded (vs. the W1a
	# placeholder fallback) — the two libraries are otherwise interchangeable.
	lib.set_meta("retargeted", true)

	var loaded: int = 0
	for key in CLIP_KEYS:
		var src: Animation = _find_clip(glb_player, key)
		if src == null:
			continue
		lib.add_animation(key, _rekey_clip(src, skeleton, skel_path, glb_rest))
		loaded += 1

	scene.free()
	if loaded != CLIP_KEYS.size():
		return null
	return lib


# Find the clip named `key` across every library on `player` (glTF import puts
# clips under the default/empty library, keyed by the Blender action name).
static func _find_clip(player: AnimationPlayer, key: String) -> Animation:
	for ln in player.get_animation_library_list():
		var lib: AnimationLibrary = player.get_animation_library(ln)
		if lib.has_animation(key):
			return lib.get_animation(key)
	return null


# Re-key one clip's ROTATION_3D tracks onto `skeleton`, rebasing glb-rest ->
# live-rest and repathing to "<skel_path>:<bone>". Non-rotation tracks and tracks
# for bones absent on the live skeleton are dropped.
static func _rekey_clip(src: Animation, skeleton: Skeleton3D, skel_path: NodePath,
		glb_rest: Dictionary) -> Animation:
	var out := Animation.new()
	out.length = src.length
	out.loop_mode = src.loop_mode
	for t in range(src.get_track_count()):
		if src.track_get_type(t) != Animation.TYPE_ROTATION_3D:
			continue
		var path: String = String(src.track_get_path(t))
		var bone: String = path.get_slice(":", path.get_slice_count(":") - 1)
		var bi: int = skeleton.find_bone(bone)
		if bi < 0:
			continue
		# q_live = live_rest * glb_rest^-1 * q_glb  (identity when rests match).
		var live_rest: Quaternion = skeleton.get_bone_rest(bi).basis.get_rotation_quaternion()
		var src_rest: Quaternion = glb_rest.get(bone, Quaternion.IDENTITY)
		var corr: Quaternion = live_rest * src_rest.inverse()

		var nt: int = out.add_track(Animation.TYPE_ROTATION_3D)
		out.track_set_path(nt, NodePath("%s:%s" % [str(skel_path), bone]))
		for k in range(src.track_get_key_count(t)):
			var tm: float = src.track_get_key_time(t, k)
			var q_glb: Quaternion = src.track_get_key_value(t, k)
			out.rotation_track_insert_key(nt, tm, (corr * q_glb).normalized())
	return out
