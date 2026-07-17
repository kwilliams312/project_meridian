# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — WASD → world-space movement basis (issue #619).
#
# CHR-02 "WoW-style" locomotion: WASD is relative to the CHARACTER's facing
# (character_yaw, committed by the RMB-steer camera — MeridianTpsCamera #105), NOT
# to the camera orbit. W drives along the character's nose, S backpedals, A/D
# strafe left/right of that nose. The visible local Body node is rotated to
# character_yaw by the camera (yaw_target), so this basis MUST match Godot's own
# Y-rotation convention exactly, or "forward" drifts away from the facing the
# player sees as soon as they steer off yaw 0 (the #619 defect).
#
# Godot is right-handed, Y-up, and a node's local FORWARD is -Z. Rotating the
# input by the Body's yaw is therefore precisely `Basis(Vector3.UP, yaw) * local`,
# which `Vector3.rotated(Vector3.UP, yaw)` computes. The previous hand-rolled
# sin/cos form used the TRANSPOSE (i.e. -yaw), mirroring travel across the world
# Z-axis for every non-zero yaw. Using the engine's own rotation guarantees the
# emitted move vector is parallel to the Body's transform basis for all yaws.
#
# Pure + engine-light (only Vector3 math) so it is unit-testable headless without
# the net stack — see input_move_verify.gd, which asserts the output against the
# REAL rotated Body node's forward/right vectors.
extends RefCounted


# Map physical WASD (as forward/strafe scalars in {-1,0,+1}) into a world-space
# ground move vector for a character facing `yaw` radians.
#   fwd    = +1 for W (toward the nose), -1 for S (backpedal), 0 otherwise
#   strafe = +1 for D (character's right), -1 for A (left), 0 otherwise
#   yaw    = the character's facing (MeridianTpsCamera.get_character_yaw()), the
#            same value written onto the Body node's rotation.y.
# Returns a Vector3 in the XZ plane (y = 0). Diagonal magnitude is left as-is
# (√2 for W+D); the movement controller core normalizes direction so diagonals
# are not faster (movement_controller.cpp integrate_tick).
static func character_relative_move(fwd: float, strafe: float, yaw: float) -> Vector3:
	# Local input in the character's frame: +X = right, -Z = forward (Godot).
	var local := Vector3(strafe, 0.0, -fwd)
	# Rotate by the Body's yaw using Godot's own Y-rotation (Basis(UP, yaw) * v),
	# so the result is parallel to the visible Body's forward/right for ALL yaws.
	return local.rotated(Vector3.UP, yaw)


# Gate a raw jump-key press into the `jump` intent fed to the controller (#905).
# Jump launches ONLY from the ground: MeridianMovementController integrate_tick
# launches on `prev.grounded && input.jump`, and encode_state_flags stamps kJump
# (bit 7) whenever `input.jump` is set — regardless of grounded. So a raw press
# while airborne would emit a spurious kJump intent to the server without ever
# launching. Gating the press on the mover's grounded state here means:
#   * grounded + pressed  → true  (launch this tick; kJump emitted, as intended)
#   * airborne + pressed  → false (NO air-jump / double-jump; no spurious kJump)
#   * not pressed         → false
# Shared (not inlined in world.gd) so _tick_local_player and its headless verify
# assert ONE definition of "may jump this tick" — same pattern as
# character_relative_move above (see jump_input_verify.gd).
static func grounded_jump(jump_pressed: bool, grounded: bool) -> bool:
	return jump_pressed and grounded
