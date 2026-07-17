# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — locomotion animation scaffold for AssembledCharacter
# (story #907, W1a of the character-animation epic #906). Builds, entirely in
# code, the client's locomotion animation system: a PLACEHOLDER AnimationLibrary
# (idle/walk/run/jump) plus the AnimationTree state machine that blends them by
# planar speed. This exists ONLY to build + prove the system; the real Mixamo
# clips (retargeted through Godot's SkeletonProfileHumanoid importer) replace the
# placeholders in Wave 3 (#906 W3-clips), gated on the provenance policy story.
#
# ── PLACEHOLDER CLIP PROVENANCE (TD-09) ─────────────────────────────────────
# The four clips are AUTHORED ORIGINAL data — a handful of keyframes rotating a
# few limbs, generated procedurally below from nothing but code. There is NO
# external source, NO license, NO AI/Meshy involvement, so `source_tier:
# original` (CONTRIBUTING.md → Asset provenance). They are stamped on the
# AnimationLibrary via set_meta("source_tier", "original"). No IF-8 asset sidecar
# is written: that machinery is for SHIPPED art/audio files, and the epic (#906)
# records that asset.schema.yaml has no animation-clip asset class yet — adding
# one + clearing the Mixamo/Adobe license is the human-gated W3-prov story. A
# code-generated placeholder that never ships needs neither.
#
# ── STATE MACHINE SHAPE ─────────────────────────────────────────────────────
# AnimationNodeStateMachine (the AnimationTree root) with THREE states:
#   * "idle"   — AnimationNodeAnimation(idle clip). Stationary.
#   * "ground" — AnimationNodeBlendSpace1D over the planar-speed axis, points
#                idle@0 / walk@kWalkSpeed / run@kRunSpeed. This is the walk↔run
#                blend: set parameters/ground/blend_position to the planar speed
#                and the node cross-blends walk↔run continuously.
#   * "air"    — AnimationNodeAnimation(jump clip). Airborne / jump.
# MoveMode (movement_constants.h:76) maps to states in AssembledCharacter.
# set_locomotion(): Idle→"idle", Walk/Run→"ground" (+blend_position=speed),
# Jump→"air", and grounded==false→"air" regardless of mode.
#
# The clips key ROTATION-3D tracks to the canonical SkeletonProfileHumanoid bone
# names the chibi/ardent rig carries (tools/blender/meridian_rig/bones.py). Only
# bones that actually resolve on the live skeleton get a track, so a rig missing
# any of them (or the capsule fallback, which has no skeleton at all) is a safe
# no-op rather than a load error.

extends RefCounted
class_name MeridianLocomotion

## Provenance tier stamped on the placeholder library (see header / TD-09).
const SOURCE_TIER: String = "original"

## State-machine node names (also the values body_skeleton()'s AnimationTree
## reports from get_current_node()). Public so the driver stories (W1b/W1c) and
## the verify reference them by name, never by literal.
const STATE_IDLE: String = "idle"
const STATE_GROUND: String = "ground"
const STATE_AIR: String = "air"

## Clip/library keys.
const CLIP_IDLE: String = "idle"
const CLIP_WALK: String = "walk"
const CLIP_RUN: String = "run"
const CLIP_JUMP: String = "jump"
const LIBRARY_NAME: String = "meridian_locomotion_placeholder"

# The blend-space anchor speeds (m/s), mirroring movement_constants.h
# [SPIKE-LOCKED] kWalkSpeed / kRunSpeed so the blend axis matches the speeds the
# driver stories feed in. Idle sits at 0.
const _WALK_SPEED: float = 2.5
const _RUN_SPEED: float = 6.0

# The canonical humanoid bones the placeholder clips animate (a subset of the 56
# SkeletonProfileHumanoid bones — bones.py). Legs swing for walk/run, arms
# counter-swing, spine gives idle a breath. Any name absent on the live skeleton
# is silently skipped (guarded in _add_swing_track).
const _LEG_L: String = "LeftUpperLeg"
const _LEG_R: String = "RightUpperLeg"
const _SHIN_L: String = "LeftLowerLeg"
const _SHIN_R: String = "RightLowerLeg"
const _ARM_L: String = "LeftUpperArm"
const _ARM_R: String = "RightUpperArm"
const _SPINE: String = "Spine"


## Build the placeholder AnimationLibrary keyed to `skeleton`'s bones. `skel_path`
## is the NodePath (from the owning AnimationPlayer's root_node) to the skeleton —
## it prefixes every bone track path ("<skel_path>:<bone>"). Returns a library
## carrying the four clips, stamped source_tier: original.
static func build_library(skeleton: Skeleton3D, skel_path: NodePath) -> AnimationLibrary:
	var lib := AnimationLibrary.new()
	lib.resource_name = LIBRARY_NAME
	lib.set_meta("source_tier", SOURCE_TIER)
	lib.add_animation(CLIP_IDLE, _make_idle(skeleton, skel_path))
	lib.add_animation(CLIP_WALK, _make_stride(skeleton, skel_path, 0.35, 1.0, false))
	lib.add_animation(CLIP_RUN, _make_stride(skeleton, skel_path, 0.7, 0.6, false))
	lib.add_animation(CLIP_JUMP, _make_jump(skeleton, skel_path))
	return lib


## Configure an AnimationTree with the state machine described in the header. The
## caller must, AFTER adding the returned tree to the scene as a sibling of the
## AnimationPlayer, set tree.anim_player to the path to that player — the path can
## only be computed once both nodes share a parent. Returns the tree inactive.
static func build_tree() -> AnimationTree:
	var tree := AnimationTree.new()
	tree.name = "LocomotionTree"

	var idle := AnimationNodeAnimation.new()
	idle.animation = _qualified(CLIP_IDLE)
	var jump := AnimationNodeAnimation.new()
	jump.animation = _qualified(CLIP_JUMP)

	# The ground blend space: idle at 0, walk at kWalkSpeed, run at kRunSpeed.
	var ground := AnimationNodeBlendSpace1D.new()
	ground.min_space = 0.0
	ground.max_space = _RUN_SPEED
	var g_idle := AnimationNodeAnimation.new()
	g_idle.animation = _qualified(CLIP_IDLE)
	var g_walk := AnimationNodeAnimation.new()
	g_walk.animation = _qualified(CLIP_WALK)
	var g_run := AnimationNodeAnimation.new()
	g_run.animation = _qualified(CLIP_RUN)
	# 4-arg form (node, pos, at_index, name): naming the points silences the
	# "empty names will be deprecated" warning (Godot 4.7 add_blend_point).
	ground.add_blend_point(g_idle, 0.0, -1, CLIP_IDLE)
	ground.add_blend_point(g_walk, _WALK_SPEED, -1, CLIP_WALK)
	ground.add_blend_point(g_run, _RUN_SPEED, -1, CLIP_RUN)

	var sm := AnimationNodeStateMachine.new()
	sm.add_node(STATE_IDLE, idle)
	sm.add_node(STATE_GROUND, ground)
	sm.add_node(STATE_AIR, jump)
	# Transitions both ways between every pair so travel() can always reach the
	# target directly (an immediate cross-fade, no intermediate hops).
	_link(sm, STATE_IDLE, STATE_GROUND)
	_link(sm, STATE_GROUND, STATE_AIR)
	_link(sm, STATE_IDLE, STATE_AIR)
	# The entry state is established at runtime via playback.start(STATE_IDLE) once
	# the tree is active (AnimationNodeStateMachine has no set_start_node in Godot
	# 4); see AssembledCharacter._build_locomotion.

	tree.tree_root = sm
	return tree


# A qualified animation name is "<library>/<clip>" once the library is added to
# the player under LIBRARY_NAME.
static func _qualified(clip: String) -> String:
	return "%s/%s" % [LIBRARY_NAME, clip]


# Add a bidirectional immediate transition between two states.
static func _link(sm: AnimationNodeStateMachine, a: String, b: String) -> void:
	var t1 := AnimationNodeStateMachineTransition.new()
	t1.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_IMMEDIATE
	t1.xfade_time = 0.15
	sm.add_transition(a, b, t1)
	var t2 := AnimationNodeStateMachineTransition.new()
	t2.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_IMMEDIATE
	t2.xfade_time = 0.15
	sm.add_transition(b, a, t2)


# Idle: a slow spine "breath" — a small, looping rotation so the clip has motion
# to prove it plays, without translating the character.
static func _make_idle(skeleton: Skeleton3D, skel_path: NodePath) -> Animation:
	var anim := Animation.new()
	anim.length = 2.0
	anim.loop_mode = Animation.LOOP_LINEAR
	_add_swing_track(anim, skeleton, skel_path, _SPINE, Vector3(1, 0, 0), 0.04, 0.0)
	return anim


# Walk/run stride: legs swing fore/aft in anti-phase, arms counter-swing, over
# `length` seconds. `amp` is the peak swing (radians) — bigger + faster for run.
static func _make_stride(skeleton: Skeleton3D, skel_path: NodePath, amp: float,
		length: float, _unused: bool) -> Animation:
	var anim := Animation.new()
	anim.length = length
	anim.loop_mode = Animation.LOOP_LINEAR
	# Legs anti-phase (R leads by half a cycle → phase 0.5).
	_add_swing_track(anim, skeleton, skel_path, _LEG_L, Vector3(1, 0, 0), amp, 0.0)
	_add_swing_track(anim, skeleton, skel_path, _LEG_R, Vector3(1, 0, 0), amp, 0.5)
	# Knees bend on the back-swing (half amplitude, double frequency reads as a
	# bend); kept crude on purpose.
	_add_swing_track(anim, skeleton, skel_path, _SHIN_L, Vector3(1, 0, 0), amp * 0.5, 0.25)
	_add_swing_track(anim, skeleton, skel_path, _SHIN_R, Vector3(1, 0, 0), amp * 0.5, 0.75)
	# Arms counter-swing the legs (L arm with R leg).
	_add_swing_track(anim, skeleton, skel_path, _ARM_L, Vector3(1, 0, 0), amp * 0.6, 0.5)
	_add_swing_track(anim, skeleton, skel_path, _ARM_R, Vector3(1, 0, 0), amp * 0.6, 0.0)
	return anim


# Jump/air: a static-ish tuck — legs rotate up and hold, arms rise. One keyframe
# past rest is enough to distinguish it from the ground clips.
static func _make_jump(skeleton: Skeleton3D, skel_path: NodePath) -> Animation:
	var anim := Animation.new()
	anim.length = 0.6
	anim.loop_mode = Animation.LOOP_NONE
	_add_pose_track(anim, skeleton, skel_path, _LEG_L, Vector3(1, 0, 0), 0.5)
	_add_pose_track(anim, skeleton, skel_path, _LEG_R, Vector3(1, 0, 0), 0.5)
	_add_pose_track(anim, skeleton, skel_path, _ARM_L, Vector3(1, 0, 0), -0.6)
	_add_pose_track(anim, skeleton, skel_path, _ARM_R, Vector3(1, 0, 0), -0.6)
	return anim


# Insert a ROTATION-3D track that swings `bone` sinusoidally about `axis` with
# peak `amp` radians, phase-shifted by `phase` cycles, keyed relative to the
# bone's REST rotation so limbs animate around their bind pose. No-op (no track
# added) when the bone is absent — the safe-fallback contract.
static func _add_swing_track(anim: Animation, skeleton: Skeleton3D, skel_path: NodePath,
		bone: String, axis: Vector3, amp: float, phase: float) -> void:
	var idx: int = skeleton.find_bone(bone)
	if idx < 0:
		return
	var rest: Quaternion = skeleton.get_bone_rest(idx).basis.get_rotation_quaternion()
	var track: int = anim.add_track(Animation.TYPE_ROTATION_3D)
	anim.track_set_path(track, NodePath("%s:%s" % [str(skel_path), bone]))
	var steps: int = 8
	for s in range(steps + 1):
		var t: float = anim.length * float(s) / float(steps)
		var theta: float = sin((float(s) / float(steps) + phase) * TAU) * amp
		var q: Quaternion = rest * Quaternion(axis.normalized(), theta)
		anim.rotation_track_insert_key(track, t, q)


# Insert a ROTATION-3D track that holds `bone` at a single `angle` (radians)
# about `axis`, relative to rest — a static pose (used by the jump tuck). No-op
# when the bone is absent.
static func _add_pose_track(anim: Animation, skeleton: Skeleton3D, skel_path: NodePath,
		bone: String, axis: Vector3, angle: float) -> void:
	var idx: int = skeleton.find_bone(bone)
	if idx < 0:
		return
	var rest: Quaternion = skeleton.get_bone_rest(idx).basis.get_rotation_quaternion()
	var track: int = anim.add_track(Animation.TYPE_ROTATION_3D)
	anim.track_set_path(track, NodePath("%s:%s" % [str(skel_path), bone]))
	var q: Quaternion = rest * Quaternion(axis.normalized(), angle)
	anim.rotation_track_insert_key(track, 0.0, q)
	anim.rotation_track_insert_key(track, anim.length, q)
