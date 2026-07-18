# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the AssembledCharacter
# LOCOMOTION scaffold (story #907, W1a of the character-animation epic #906). NOT
# a shipped scene: a SceneTree script run
#   godot --headless --path client/project --import   # once, to seed the class cache
#   godot --headless --path client/project --script res://characters/assembled_character_locomotion_verify.gd
# (same convention as assembled_character_verify.gd). Proves, with no render and
# no server, that assemble() attaches an AnimationTree state machine to the live
# canonical skeleton, that the placeholder clip library is authored + stamped, and
# that set_locomotion(mode, planar_speed, grounded) drives the machine to the
# matching LIVE state, blends walk↔run by speed, and actually poses the skeleton.
#
# ⛔ CHECK-COUNT GUARD: a client verify that fails to LOAD (missing framework /
# stale class cache) can still exit 0 with zero assertions — a false green. This
# script counts every check and FAILS if fewer than MIN_CHECKS ran, and fails on
# any failed assertion. NEVER trust the exit code alone; read the PASS/FAIL lines.
#
# Exits 0 only when every check passed AND at least MIN_CHECKS checks ran.

extends SceneTree

const ContentDbScript := preload("res://content/content_db.gd")
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")
const LocomotionScript := preload("res://characters/locomotion.gd")
const LocomotionClipsScript := preload("res://characters/locomotion_clips.gd")

# The MoveMode ints (movement_constants.h:76), mirrored by AssembledCharacter.
const MODE_IDLE: int = 0
const MODE_WALK: int = 1
const MODE_RUN: int = 2
const MODE_JUMP: int = 3

const WALK_SPEED: float = 2.5
const RUN_SPEED: float = 6.0

# Floor on the number of checks that MUST run — a hard guard against a load
# failure (or a phase silently early-returning) reducing the suite to a
# false green. Raised for W2a (#914): the retarget phase adds ~8 checks, so the
# floor now also guarantees that phase actually ran (37 run today; a missing
# retarget phase would drop well below this).
const MIN_CHECKS: int = 34

var _fails: int = 0
var _ran: int = 0


func _check(name: String, ok: bool) -> void:
	_ran += 1
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


# _initialize kicks off the coroutine; the real work AWAITS process frames so the
# AnimationTree (an AnimationMixer) enters the scene tree and builds its bone-track
# caches — nodes added under `root` in _initialize are not yet "inside the tree",
# and the mixer never poses the skeleton until it is + a frame has ticked. The
# running app always provides this; the headless verify must await it explicitly.
func _initialize() -> void:
	_run()


func _run() -> void:
	print("meridian AssembledCharacter LOCOMOTION verify (#907)")

	var db = ContentDbScript.instance()
	_check("staged core pack is loaded", db.is_loaded())

	var ac = AssembledCharacterScript.new()
	ac.name = "assembled_character_locomotion_under_test"
	root.add_child(ac)
	await process_frame   # ac is now genuinely inside the scene tree

	_verify_attach_and_library(ac)   # assemble() builds + attaches the tree here
	await process_frame              # the AnimationMixer builds its track caches
	await process_frame
	_verify_state_transitions(ac)
	_verify_walk_run_blend(ac)
	_verify_clip_poses_skeleton(ac)
	_verify_retarget_clips(ac)
	_verify_capsule_noop(ac)

	root.remove_child(ac)
	ac.free()

	# --- CHECK-COUNT GUARD ----------------------------------------------------
	var enough: bool = _ran >= MIN_CHECKS
	print("  [%s] check-count guard — %d checks ran (>= %d required)"
		% ["PASS" if enough else "FAIL", _ran, MIN_CHECKS])
	if not enough:
		_fails += 1

	print("\n%s" % ("ALL LOCOMOTION CHECKS PASS (%d checks)" % _ran if _fails == 0
		else "%d LOCOMOTION FAILURE(S) of %d checks" % [_fails, _ran]))
	quit(0 if _fails == 0 else 1)


# Step the AnimationTree deterministically so travel()s settle and clips advance.
func _settle(tree: AnimationTree, dt: float, steps: int) -> void:
	for _i in range(steps):
		tree.advance(dt)


# --- A. attach + placeholder library ------------------------------------------
func _verify_attach_and_library(ac) -> void:
	print(" assemble(1, 0) then inspect the locomotion scaffold:")
	var ok: bool = ac.assemble(1, 0, {"hair": 1, "face": 1, "skin": 1}, [])
	_check("assemble returns true", ok and ac.is_assembled())
	var skel: Skeleton3D = ac.body_skeleton()
	_check("body skeleton present", skel != null)

	var tree: AnimationTree = ac.locomotion_tree()
	_check("assemble attached an AnimationTree", tree != null)
	if tree == null:
		return
	_check("AnimationTree is active", tree.active)
	var sm := tree.tree_root as AnimationNodeStateMachine
	_check("tree root is an AnimationNodeStateMachine", sm != null)
	if sm != null:
		_check("state machine has the idle/ground/air states",
			sm.has_node(LocomotionScript.STATE_IDLE)
			and sm.has_node(LocomotionScript.STATE_GROUND)
			and sm.has_node(LocomotionScript.STATE_AIR))
		_check("the ground state is a BlendSpace1D (walk↔run blend)",
			sm.get_node(LocomotionScript.STATE_GROUND) is AnimationNodeBlendSpace1D)

	# The placeholder AnimationLibrary: four clips, keyed to the humanoid rig,
	# stamped source_tier: original (authored, no license, no AI — TD-09).
	var players: Array = ac.find_children("*", "AnimationPlayer", true, false)
	_check("assemble attached an AnimationPlayer", players.size() == 1)
	if players.size() == 1:
		var player: AnimationPlayer = players[0]
		var lib_name: String = LocomotionScript.LIBRARY_NAME
		_check("the placeholder library is present on the player",
			player.has_animation_library(lib_name))
		if player.has_animation_library(lib_name):
			var lib: AnimationLibrary = player.get_animation_library(lib_name)
			_check("library carries the four idle/walk/run/jump clips",
				lib.has_animation(LocomotionScript.CLIP_IDLE)
				and lib.has_animation(LocomotionScript.CLIP_WALK)
				and lib.has_animation(LocomotionScript.CLIP_RUN)
				and lib.has_animation(LocomotionScript.CLIP_JUMP))
			_check("library is stamped source_tier: original (provenance TD-09)",
				String(lib.get_meta("source_tier", "")) == "original")
			# Clips key to the SkeletonProfileHumanoid bone names the rig uses:
			# the walk clip's first track path must resolve to a real bone.
			var walk: Animation = lib.get_animation(LocomotionScript.CLIP_WALK)
			var tracks_ok: bool = walk.get_track_count() > 0
			for t in range(walk.get_track_count()):
				var p: String = String(walk.track_get_path(t))
				var bone: String = p.get_slice(":", p.get_slice_count(":") - 1)
				if skel == null or skel.find_bone(bone) < 0:
					tracks_ok = false
			_check("every walk-clip track keys to a real humanoid bone on the skeleton",
				tracks_ok)


# --- B. set_locomotion drives the LIVE state machine --------------------------
func _verify_state_transitions(ac) -> void:
	print(" set_locomotion drives the AnimationTree to the matching live state:")
	var tree: AnimationTree = ac.locomotion_tree()
	if tree == null:
		_check("locomotion tree present for transition checks", false)
		return
	var playback: AnimationNodeStateMachinePlayback = tree.get("parameters/playback")
	_check("state-machine playback is reachable", playback != null)
	if playback == null:
		return

	# Idle.
	ac.set_locomotion(MODE_IDLE, 0.0, true)
	_settle(tree, 0.2, 4)
	_check("set_locomotion(Idle) → current state is 'idle'",
		String(playback.get_current_node()) == LocomotionScript.STATE_IDLE)

	# Walk (grounded) → ground blend state.
	ac.set_locomotion(MODE_WALK, WALK_SPEED, true)
	_settle(tree, 0.2, 4)
	_check("set_locomotion(Walk) → current state is 'ground'",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)

	# Run (grounded) → still the ground blend state (walk↔run live in it).
	ac.set_locomotion(MODE_RUN, RUN_SPEED, true)
	_settle(tree, 0.2, 4)
	_check("set_locomotion(Run) → current state is 'ground'",
		String(playback.get_current_node()) == LocomotionScript.STATE_GROUND)

	# grounded == false with a ground mode → air state (jump selected by grounded).
	ac.set_locomotion(MODE_WALK, WALK_SPEED, false)
	_settle(tree, 0.2, 4)
	_check("set_locomotion(_, _, grounded=false) → current state is 'air'",
		String(playback.get_current_node()) == LocomotionScript.STATE_AIR)

	# Explicit Jump mode → air state too.
	ac.set_locomotion(MODE_JUMP, RUN_SPEED, true)
	_settle(tree, 0.2, 4)
	_check("set_locomotion(Jump) → current state is 'air'",
		String(playback.get_current_node()) == LocomotionScript.STATE_AIR)

	# Back to idle so later phases start clean.
	ac.set_locomotion(MODE_IDLE, 0.0, true)
	_settle(tree, 0.2, 4)


# --- C. walk↔run blend tracks planar_speed ------------------------------------
func _verify_walk_run_blend(ac) -> void:
	print(" the walk↔run blend position tracks planar_speed:")
	var tree: AnimationTree = ac.locomotion_tree()
	if tree == null:
		_check("locomotion tree present for blend checks", false)
		return
	var param: String = "parameters/%s/blend_position" % LocomotionScript.STATE_GROUND

	ac.set_locomotion(MODE_WALK, WALK_SPEED, true)
	_settle(tree, 0.1, 2)
	var at_walk: float = float(tree.get(param))
	_check("blend_position == walk speed (2.5) after set_locomotion(Walk, 2.5)",
		is_equal_approx(at_walk, WALK_SPEED))

	ac.set_locomotion(MODE_RUN, RUN_SPEED, true)
	_settle(tree, 0.1, 2)
	var at_run: float = float(tree.get(param))
	_check("blend_position == run speed (6.0) after set_locomotion(Run, 6.0)",
		is_equal_approx(at_run, RUN_SPEED))

	# An intermediate speed lands between the walk and run anchors — a real blend.
	var mid: float = (WALK_SPEED + RUN_SPEED) * 0.5
	ac.set_locomotion(MODE_RUN, mid, true)
	_settle(tree, 0.1, 2)
	var at_mid: float = float(tree.get(param))
	_check("blend_position tracks an intermediate speed (walk < mid < run)",
		at_mid > WALK_SPEED and at_mid < RUN_SPEED and is_equal_approx(at_mid, mid))

	ac.set_locomotion(MODE_IDLE, 0.0, true)
	_settle(tree, 0.2, 4)


# --- D. the clips actually pose the live skeleton -----------------------------
# The strongest guard: if the clip track paths were wrong (a misresolved skeleton
# path), the AnimationTree would write nothing and every bone would sit at rest.
# Sampling the PEAK deviation across a full cycle (not a single instant) is robust
# to where the looping clip's phase happens to land — the clip time is continuous
# across state re-entries by design (no foot-pop), so a single snapshot can catch
# a zero-crossing. Walk must swing the legs; idle must move the spine; and the two
# states must be distinct (walk swings the leg far more than idle does).
func _verify_clip_poses_skeleton(ac) -> void:
	print(" the placeholder clips actually pose the live skeleton:")
	var tree: AnimationTree = ac.locomotion_tree()
	var skel: Skeleton3D = ac.body_skeleton()
	if tree == null or skel == null:
		_check("tree + skeleton present for pose check", false)
		return
	var leg: int = skel.find_bone("LeftUpperLeg")
	var spine: int = skel.find_bone("Spine")
	_check("LeftUpperLeg + Spine bones exist on the rig", leg >= 0 and spine >= 0)
	if leg < 0 or spine < 0:
		return

	# Walk: peak leg swing over a full 1.0s cycle must be clearly non-zero.
	ac.set_locomotion(MODE_WALK, WALK_SPEED, true)
	var walk_leg_peak: float = _peak_swing(tree, skel, leg, 1.2, 0.05)
	_check("walk clip swings LeftUpperLeg (peak %.3f rad over a cycle)" % walk_leg_peak,
		walk_leg_peak > 0.05)

	# Idle: the spine "breath" track must animate the spine (proves the idle clip
	# plays too), while the LEG stays essentially still (idle has no leg track).
	ac.set_locomotion(MODE_IDLE, 0.0, true)
	var idle_spine_peak: float = _peak_swing(tree, skel, spine, 2.2, 0.05)
	_check("idle clip moves the Spine (peak %.3f rad over a cycle)" % idle_spine_peak,
		idle_spine_peak > 0.005)
	var idle_leg_peak: float = _peak_swing(tree, skel, leg, 2.2, 0.05)
	_check("idle keeps the legs far calmer than walk (idle leg << walk leg)",
		idle_leg_peak < walk_leg_peak * 0.5)


# Advance the tree over `dur` seconds in `dt` steps, returning the peak
# angle between the bone's live pose and its rest rotation across the window.
func _peak_swing(tree: AnimationTree, skel: Skeleton3D, bone_idx: int,
		dur: float, dt: float) -> float:
	var rest: Quaternion = skel.get_bone_rest(bone_idx).basis.get_rotation_quaternion()
	var peak: float = 0.0
	var elapsed: float = 0.0
	while elapsed < dur:
		tree.advance(dt)
		elapsed += dt
		peak = max(peak, rest.angle_to(skel.get_bone_pose_rotation(bone_idx)))
	return peak


# --- D2. the RETARGETED clips (W2a #914) — the NON-VACUOUS retarget proof ------
# The strongest guards against a false-green retarget. A fixture authored on a
# DELIBERATELY non-canonical source (mixamorig:-prefixed names + an A-pose rest)
# is loaded, retargeted, and re-keyed onto the live rig; a no-op / no-retarget
# path fails LOUDLY here:
#   (a) POST-retarget track paths carry CANONICAL SkeletonProfileHumanoid names —
#       every source "mixamorig:" name is GONE. (Fails if the name remap skipped.)
#   (b) GOLDEN POSE per clip — the fixture holds LeftUpperArm STRAIGHT UP at t=0.
#       On the canonical rig the left upper arm RESTS horizontal, so "up" is only
#       reachable when the A-pose→T-pose rest was actually reconciled. We sample
#       the upper-arm WORLD direction (LeftLowerArm origin − LeftUpperArm origin);
#       a no-op that copied the source's A-pose-relative local rotations onto the
#       T-pose rig would point the arm elsewhere and miss the landmark. (Fails if
#       the rest reconciliation was skipped.)
func _verify_retarget_clips(ac) -> void:
	print(" the RETARGETED fixture clips (W2a #914) load, carry canonical names, hit the golden pose:")
	var players: Array = ac.find_children("*", "AnimationPlayer", true, false)
	if players.size() != 1:
		_check("locomotion player present for retarget checks", false)
		return
	var player: AnimationPlayer = players[0]
	var skel: Skeleton3D = ac.body_skeleton()
	var lib_name: String = LocomotionScript.LIBRARY_NAME
	if not player.has_animation_library(lib_name) or skel == null:
		_check("retargeted library + skeleton present", false)
		return
	var lib: AnimationLibrary = player.get_animation_library(lib_name)

	# The staged glb actually loaded + retargeted (not the W1a placeholder
	# fallback). If this fails the fixture/pipeline is broken — the whole point of
	# W2a — so the rest of the phase is meaningless; report and bail.
	var retargeted: bool = bool(lib.get_meta("retargeted", false))
	_check("the RETARGETED library loaded (staged glb, not the placeholder fallback)",
		retargeted)
	if not retargeted:
		return

	# (a) Every clip's every rotation track keys to a CANONICAL bone; no source
	# "mixamorig:" name survives anywhere.
	var all_canonical: bool = true
	var mixamo_seen: bool = false
	for clip in LocomotionClipsScript.CLIP_KEYS:
		if not lib.has_animation(clip):
			all_canonical = false
			continue
		var anim: Animation = lib.get_animation(clip)
		for t in range(anim.get_track_count()):
			var p: String = String(anim.track_get_path(t))
			if p.contains("mixamorig"):
				mixamo_seen = true
			var bone: String = p.get_slice(":", p.get_slice_count(":") - 1)
			if skel.find_bone(bone) < 0:
				all_canonical = false
	_check("no source 'mixamorig:' bone name survives the retarget", not mixamo_seen)
	_check("every retargeted track keys to a canonical bone on the live skeleton",
		all_canonical)

	# (b) Golden pose per clip. Seek each clip to t=0 on the player and read the
	# LEFT UPPER ARM world direction. Isolate the player by silencing the tree so
	# only the seeked clip poses the skeleton.
	var tree: AnimationTree = ac.locomotion_tree()
	var was_active: bool = tree != null and tree.active
	if tree != null:
		tree.active = false
	var upper: int = skel.find_bone("LeftUpperArm")
	var lower: int = skel.find_bone("LeftLowerArm")
	_check("LeftUpperArm + LeftLowerArm exist (golden-landmark bones)",
		upper >= 0 and lower >= 0)
	if upper >= 0 and lower >= 0:
		for clip in LocomotionClipsScript.CLIP_KEYS:
			if not lib.has_animation(clip):
				_check("golden pose — clip '%s' present" % clip, false)
				continue
			player.play("%s/%s" % [lib_name, clip])
			player.seek(0.0, true)
			var up_o: Vector3 = skel.get_bone_global_pose(upper).origin
			var lo_o: Vector3 = skel.get_bone_global_pose(lower).origin
			var dir: Vector3 = (lo_o - up_o).normalized()
			var dot: float = dir.dot(Vector3.UP)
			_check("golden pose — clip '%s': LeftUpperArm points UP at t=0 (dot %.3f > 0.8)"
				% [clip, dot], dot > 0.8)
		player.stop()
	if tree != null:
		tree.active = was_active


# --- E. capsule fallback / unassembled → safe no-op ---------------------------
# A catalog miss returns false and the caller keeps its capsule (no skeleton).
# set_locomotion on such an instance must be a silent no-op, not a crash, and no
# AnimationTree is built.
func _verify_capsule_noop(_ac) -> void:
	print(" capsule fallback (no skeleton) → set_locomotion is a safe no-op:")
	var fresh = AssembledCharacterScript.new()
	fresh.name = "capsule_fallback_under_test"
	root.add_child(fresh)
	var ok: bool = fresh.assemble(3, 0, {}, [])   # Sylvane (id 3): no staged catalog
	_check("catalog miss → assemble returns false (capsule fallback)",
		not ok and not fresh.is_assembled())
	_check("no AnimationTree built on the capsule fallback",
		fresh.locomotion_tree() == null)
	# Must not crash / must not raise — call every mode.
	fresh.set_locomotion(MODE_IDLE, 0.0, true)
	fresh.set_locomotion(MODE_WALK, WALK_SPEED, true)
	fresh.set_locomotion(MODE_RUN, RUN_SPEED, true)
	fresh.set_locomotion(MODE_JUMP, 0.0, false)
	_check("set_locomotion on the capsule fallback is a safe no-op (no crash)", true)
	root.remove_child(fresh)
	fresh.free()
