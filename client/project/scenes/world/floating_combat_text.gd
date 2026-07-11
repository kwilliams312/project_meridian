# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — POOLED floating combat text (CMB-04, #530; combat-presentation
# epic #23). A fixed pool of billboarded Label3D nodes that render the rising, fading
# damage/heal/miss numbers a CAST_RESULT (0x3004) resolves to, positioned over the
# target entity.
#
# PURE PRESENTATION — server-authoritative (Principle 1): this node NEVER computes a
# gameplay number. It only DISPLAYS what CAST_RESULT already decided (the attack-table
# outcome + the damage/heal amount). The world scene subscribes to the event bus's
# cast_result_received seam and calls spawn_over() with the target's world position.
#
# NO per-hit allocation: POOL_SIZE Label3D children are built ONCE in _ready() and
# recycled forever. spawn_over() pulls a free label, drives it, and returns it to the
# free list when its lifetime elapses — so a burst of hits reuses the same nodes and a
# steady fight never churns the heap. When the pool is momentarily exhausted the OLDEST
# active label is recycled early (a dropped number is invisible for a frame, never a
# leak or a stall).
#
# The animation is time-driven in _advance(delta) (called from _process). A headless
# test drives _advance() directly with a large dt to prove recycling without waiting
# real seconds (see combat_text_verify.gd).

extends Node3D

# --- Attack-table outcomes (mirrors schema/net/world.fbs AttackOutcome 1:1) ---
const OUTCOME_MISS := 0
const OUTCOME_DODGE := 1
const OUTCOME_PARRY := 2
const OUTCOME_HIT := 3
const OUTCOME_CRIT := 4

# --- Pool + animation tuning --------------------------------------------------
const POOL_SIZE := 24        # concurrent numbers before the oldest is recycled early
const LIFETIME := 1.1        # seconds a number lives (rise + fade)
const RISE_METRES := 1.6     # how far a number floats up over its lifetime
const HEAD_OFFSET := 2.4     # metres above the target node origin the number starts
const PIXEL_SIZE := 0.008    # Label3D world scale (metres per font pixel)
const FONT_SIZE_BASE := 48   # normal hit / heal / miss
const FONT_SIZE_CRIT := 72   # a crit reads BIGGER (server flagged CRIT)

# Outcome/heal → color. Damage is warm, a crit is hot-orange, a heal is green, and an
# avoided hit (miss/dodge/parry) is a muted gray word.
const COL_DAMAGE := Color(1.0, 0.92, 0.55)
const COL_CRIT := Color(1.0, 0.55, 0.12)
const COL_HEAL := Color(0.36, 1.0, 0.45)
const COL_CRIT_HEAL := Color(0.55, 1.0, 0.40)
const COL_AVOID := Color(0.78, 0.78, 0.82)

# One live number: the Label3D, its spawn world position, and how long it has run.
class _Active:
	var label: Label3D
	var start_pos: Vector3
	var elapsed: float

var _free: Array[Label3D] = []      # recycled, hidden labels ready to reuse
var _active: Array = []             # Array[_Active] currently animating


func _ready() -> void:
	# Build the pool ONCE. Every label is a billboarded, depth-test-off Label3D that
	# always draws on top of the world geometry, hidden until spawned.
	for i in range(POOL_SIZE):
		var label := Label3D.new()
		label.name = "CombatText_%d" % i
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.no_depth_test = true
		label.fixed_size = false
		label.pixel_size = PIXEL_SIZE
		label.render_priority = 2       # over the guid nameplate (which uses the default 0)
		label.outline_size = 12
		label.outline_modulate = Color(0, 0, 0, 0.85)
		label.visible = false
		add_child(label)
		_free.push_back(label)


# Spawn a number over a target's WORLD position. `world_pos` is the target node's global
# origin; the number floats up from HEAD_OFFSET above it. `outcome` is the AttackOutcome
# and `is_heal` selects the heal styling. Returns the Label3D driving the number (or the
# recycled one when the pool was exhausted) — never null so callers/tests can inspect it.
func spawn_over(world_pos: Vector3, amount: int, outcome: int, is_heal: bool) -> Label3D:
	var label := _take_label()

	var avoided := (outcome == OUTCOME_MISS or outcome == OUTCOME_DODGE
		or outcome == OUTCOME_PARRY)
	var is_crit := (outcome == OUTCOME_CRIT)

	if avoided:
		label.text = _avoid_word(outcome)
		label.modulate = COL_AVOID
		label.font_size = FONT_SIZE_BASE
	elif is_heal:
		label.text = "+%d" % amount
		label.modulate = COL_CRIT_HEAL if is_crit else COL_HEAL
		label.font_size = FONT_SIZE_CRIT if is_crit else FONT_SIZE_BASE
	else:
		label.text = "%d" % amount
		label.modulate = COL_CRIT if is_crit else COL_DAMAGE
		label.font_size = FONT_SIZE_CRIT if is_crit else FONT_SIZE_BASE

	var start := world_pos + Vector3(0.0, HEAD_OFFSET, 0.0)
	label.global_position = start
	label.visible = true

	var entry := _Active.new()
	entry.label = label
	entry.start_pos = start
	entry.elapsed = 0.0
	_active.push_back(entry)
	return label


func _process(delta: float) -> void:
	_advance(delta)


# Advance every live number by `delta` seconds: float it up and fade it out, recycling
# any whose lifetime has elapsed. Split out from _process so a headless test can step the
# animation deterministically (drive a full LIFETIME in one call).
func _advance(delta: float) -> void:
	if _active.is_empty():
		return
	var i := _active.size() - 1
	while i >= 0:
		var entry: _Active = _active[i]
		entry.elapsed += delta
		var t := entry.elapsed / LIFETIME
		if t >= 1.0:
			_recycle_at(i)
		else:
			var label := entry.label
			label.global_position = entry.start_pos + Vector3(0.0, RISE_METRES * t, 0.0)
			var col := label.modulate
			col.a = 1.0 - t       # linear fade to invisible at end of life
			label.modulate = col
		i -= 1


# --- Introspection (for verification / debug) --------------------------------

func active_count() -> int:
	return _active.size()

func free_count() -> int:
	return _free.size()

func pool_size() -> int:
	return POOL_SIZE

# The most recently spawned live label (null when none active) — lets a test inspect the
# number the bus→world path floated without threading the return value back through a signal.
func newest_active_label() -> Label3D:
	if _active.is_empty():
		return null
	var entry: _Active = _active.back()
	return entry.label


# --- Pool internals ----------------------------------------------------------

# Pull a hidden label from the free list; when the pool is exhausted, recycle the OLDEST
# active number so a spawn never allocates and never fails.
func _take_label() -> Label3D:
	if _free.is_empty():
		_recycle_at(0)   # oldest active (index 0 is the earliest pushed)
	return _free.pop_back()


# Return the active entry at `index` to the free pool (hidden, reset alpha).
func _recycle_at(index: int) -> void:
	var entry: _Active = _active[index]
	_active.remove_at(index)
	var label := entry.label
	label.visible = false
	var col := label.modulate
	col.a = 1.0
	label.modulate = col
	_free.push_back(label)


func _avoid_word(outcome: int) -> String:
	match outcome:
		OUTCOME_DODGE:
			return "Dodge"
		OUTCOME_PARRY:
			return "Parry"
		_:
			return "Miss"
