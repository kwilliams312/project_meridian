# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — LOCAL-PLAYER locomotion driver (story #908, W1b of the
# character-animation epic #906). A pure, side-effect-free mapping from the
# movement mover's per-tick outputs to the (mode, planar_speed, grounded) triple
# AssembledCharacter.set_locomotion (W1a, #907) consumes.
#
# world.gd:_tick_local_player calls drive() every tick with the local player's live
# mover; the headless verify (world_local_locomotion_drive_verify.gd) drives a REAL
# MeridianMovementController through each mode and asserts the resulting live
# AnimationTree state — so this ONE mapping is exactly what ships AND what is tested,
# never a re-derivation that could drift from (or be wrong alongside) the shipped
# path.
#
# ── WHY A SEPARATE MODULE ─────────────────────────────────────────────────────
# world.gd is the scene god-object; a movement-state → animation-state decode is
# neither scene state nor rendering, and it must be exercisable headless without
# standing up the whole world scene + a net connection. Isolating it here gives the
# verify a clean seam and keeps world.gd's tick to a single call.

extends RefCounted
class_name MeridianLocomotionDriver

# The low 3 bits of the wire `state_flags` bitfield are the active MoveMode. This
# mirrors movement_constants.h:148 (kStateFlagsModeMask = 0x7) — the SAME mask the
# server reads (state_flags & 0x7) and the C++ mode_from_state_flags decodes. The
# extension does not expose the mask to GDScript, so it is duplicated here with this
# citation, exactly as AssembledCharacter.MOVE_* mirrors the MoveMode enum
# (movement_constants.h:76). Keep in sync if the wire encoding ever changes.
const STATE_FLAGS_MODE_MASK: int = 0x7

# MoveMode ints (movement_constants.h:76) — the SAME values AssembledCharacter.MOVE_*
# switches on in set_locomotion; passed straight through.
const MODE_IDLE: int = 0
const MODE_WALK: int = 1
const MODE_RUN: int = 2
const MODE_JUMP: int = 3


## Decode the MoveMode the low 3 bits of `state_flags` carry. Unknown encodings —
## only 4..7 are representable in 3 bits and none is a legal M0/M1 MoveMode — fall
## back to Run, matching the C++ mode_from_state_flags safest-default
## (movement_constants.h mode_from_state_flags). For the animation this only ever
## picks the ground blend over idle; it never mis-selects air (that is grounded).
static func mode_from_state_flags(state_flags: int) -> int:
	var m: int = state_flags & STATE_FLAGS_MODE_MASK
	if m == MODE_IDLE or m == MODE_WALK or m == MODE_RUN or m == MODE_JUMP:
		return m
	return MODE_RUN


## The planar (horizontal, XZ) speed magnitude in m/s from the mover's predicted
## velocity — the walk↔run blend axis. The vertical (Y, jump-arc) component is
## deliberately excluded: the ground blend space is a ground-speed axis, and the
## air state is selected by `grounded`, not by vertical speed.
static func planar_speed(velocity: Vector3) -> float:
	return Vector2(velocity.x, velocity.z).length()


## Drive `body`'s locomotion animation from one tick of `mover` output: decode the
## mode from `state_flags`, read the planar speed from the mover's predicted
## velocity, and take grounded straight from the mover, then call set_locomotion.
## A safe no-op when `body` is null or has no set_locomotion — the capsule fallback
## (a plain MeshInstance3D) built when appearance is absent / assembly fails.
static func drive(body: Node, mover: Object, state_flags: int) -> void:
	if body == null or not body.has_method("set_locomotion"):
		return
	body.set_locomotion(
		mode_from_state_flags(state_flags),
		planar_speed(mover.get_predicted_velocity()),
		mover.is_grounded())
