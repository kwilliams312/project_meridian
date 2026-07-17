# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — REMOTE-ENTITY locomotion driver (story #909, W1c of the
# character-animation epic #906). Drives a remote's AssembledCharacter locomotion
# animation from its INTERPOLATED motion, since — unlike the LOCAL player (#908) —
# remotes have NO MoveMode/state_flags on the wire: EntityUpdate carries only
# x/y/z/orientation (schema/net/world.fbs:338-347). So the mode + speed the
# AssembledCharacter.set_locomotion API (W1a, #907) consumes are DERIVED here by
# differencing the interpolated position the MeridianRemoteInterpolator produces each
# tick.
#
# world.gd:_update_remotes owns one instance of this per remote node and calls drive()
# every physics tick with that remote's freshly-sampled interpolated position and the
# tick dt; the headless verify (world_remote_locomotion_drive_verify.gd) drives a REAL
# AssembledCharacter through stationary / walk / run motion and asserts the resulting
# live AnimationTree state — so this ONE derivation is exactly what ships AND what is
# tested.
#
# ── WHY STATEFUL (unlike the LOCAL driver) ────────────────────────────────────
# The local driver (locomotion_driver.gd) is a pure static mapping: the mover already
# hands it state_flags + a predicted velocity, so it holds nothing. A remote has no
# velocity on the wire and none exposed by the interpolator (its API is position-only:
# get_interpolated_position / sample_entity). Recovering a velocity therefore requires
# remembering the previous position, so each remote needs its OWN instance carrying that
# per-entity state. world.gd keys one per guid alongside _remote_nodes.
#
# ── ANTI-JITTER (idle deadband + EMA smoothing) ───────────────────────────────
# Two guards keep interpolation noise from twitching a standing remote into a walk and
# from making the walk↔run blend flicker:
#   * IDLE_SPEED_EPSILON — a small deadband (m/s). A derived speed under it reads as
#     Idle, so sub-threshold position wobble on a stationary remote renders 'idle', not
#     a jittery 'walk'. Chosen well under the walk anchor (2.5 m/s) so real walking is
#     never suppressed.
#   * SPEED_SMOOTHING — an exponential moving average on the derived speed. A single
#     noisy frame (or an extrapolation/snapshot-boundary blip) is damped instead of
#     spiking the blend position. The EMA converges to a constant real speed, so steady
#     walking/running still lands on the correct blend anchor.

extends RefCounted
class_name MeridianRemoteLocomotionDriver

# MoveMode ints (movement_constants.h:76) — the SAME values AssembledCharacter.MOVE_*
# switches on in set_locomotion. Remotes never JUMP (no jump on the wire), so only
# Idle/Walk/Run are ever produced here.
const MODE_IDLE: int = 0
const MODE_WALK: int = 1
const MODE_RUN: int = 2

# The walk↔run blend anchors (m/s), mirroring locomotion.gd _WALK_SPEED / _RUN_SPEED
# (themselves the [SPIKE-LOCKED] movement_constants.h caps). The walk/run split sits at
# their midpoint — a labelling only; set_locomotion routes BOTH Walk and Run to the
# 'ground' blend space and lets blend_position (the derived speed) cross-blend them, so
# this split just names the mode, it does not gate the animation.
const WALK_SPEED: float = 2.5
const RUN_SPEED: float = 6.0
const _WALK_RUN_SPLIT: float = (WALK_SPEED + RUN_SPEED) * 0.5

# Idle deadband (m/s): derived planar speed below this reads as Idle. Sized to swallow
# interpolation noise on a stationary remote while staying far under the walk anchor so
# genuine slow walking is never mistaken for idle.
const IDLE_SPEED_EPSILON: float = 0.35

# EMA weight on the newest raw sample (0..1). Higher = snappier/noisier, lower =
# smoother/laggier. 0.35 damps single-frame interpolation blips while still converging
# to a steady real speed within a handful of ticks.
const SPEED_SMOOTHING: float = 0.35

# Per-remote state: the previous interpolated planar (XZ) position and the smoothed
# derived speed. _has_prev gates the first observe (no delta computable yet).
var _has_prev: bool = false
var _prev_planar: Vector2 = Vector2.ZERO
var _speed: float = 0.0


## Classify a derived planar speed into a MoveMode. Below the idle deadband → Idle;
## below the walk/run midpoint → Walk; else Run. Static + pure so the verify can assert
## the thresholds directly. NOTE set_locomotion treats Walk and Run identically (both →
## the 'ground' blend space); the split only labels the mode for readability/telemetry.
static func mode_for_speed(speed: float) -> int:
	if speed < IDLE_SPEED_EPSILON:
		return MODE_IDLE
	if speed < _WALK_RUN_SPLIT:
		return MODE_WALK
	return MODE_RUN


## Fold one interpolated planar position into the smoothed speed estimate and return it.
## `position` is the remote's current interpolated position (only X/Z are used — the
## vertical/jump-arc component is excluded from the ground-speed axis, exactly as the
## local driver's planar_speed does); `dt` is the sim-tick delta the position advanced
## over (world.gd TICK_MS in seconds). The first call only seeds the previous position
## (no delta yet) and reports 0.
func observe(position: Vector3, dt: float) -> float:
	var planar := Vector2(position.x, position.z)
	if not _has_prev or dt <= 0.0:
		_prev_planar = planar
		_has_prev = true
		return _speed
	var raw: float = (planar - _prev_planar).length() / dt
	_prev_planar = planar
	# EMA toward the raw sample — damps single-tick interpolation blips.
	_speed = lerpf(_speed, raw, SPEED_SMOOTHING)
	return _speed


## The current smoothed planar speed (m/s). Read-side helper for the headless verify.
func smoothed_speed() -> float:
	return _speed


## Drive `body`'s locomotion animation from this tick's interpolated `position`: derive
## the smoothed planar speed, classify it into a mode, and call set_locomotion with
## grounded ALWAYS true (remotes have no jump on the wire, so they are only ever 'idle'
## or 'ground', never 'air' — the optional authoritative remote mode+jump is the
## Wopt-wire follow-up). A safe no-op when `body` is null or has no set_locomotion — the
## capsule fallback (a plain MeshInstance3D) built when appearance is absent / assembly
## fails. The position is still folded into the estimate first so state stays continuous
## even across a body that cannot animate.
func drive(body: Node, position: Vector3, dt: float) -> void:
	var speed: float = observe(position, dt)
	if body == null or not body.has_method("set_locomotion"):
		return
	body.set_locomotion(mode_for_speed(speed), speed, true)
