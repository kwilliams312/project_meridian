# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — POOLED nameplate manager (CMB-03, #535; combat-presentation epic #23).
# Owns a fixed pool of MeridianNameplate widgets and attaches one over each VISIBLE remote
# entity, recycling it when the entity leaves AoI — so a churning scene reuses the same
# nodes instead of allocating a plate per spawn (mirrors the floating-combat-text pool #530).
#
# OWNERSHIP / TRACKING: the world scene owns the entity Node3D map (#496); it calls attach()
# with the entity's node, and the manager REPARENTS a pooled plate under that node so the
# plate tracks the entity as it moves (billboarding is GPU-side — no per-frame reposition).
# On recycle() the plate is reparented BACK to the pool BEFORE the entity node is freed, so
# the pooled node survives the entity's despawn. The LOCAL player never enters this map
# (world.gd guards `guid == _my_guid`), so its own nameplate is suppressed for free.
#
# SERVER IS LAW: the manager only forwards the name (ENTITY_ENTER) + current/max health
# (VITALS_UPDATE, via the event bus's entity_vitals_changed seam) to the plate. It invents
# nothing.
#
# DISTANCE FADE/CULL (perf): the ONLY per-frame work — _process walks the ACTIVE plates
# (a handful within AoI), fading each toward transparent past FADE_START and hiding it past
# CULL_DIST, measured from the active Camera3D. With no camera (a headless verify with no
# viewport camera) it leaves plates fully visible so the attach/track/recycle logic still
# proves out.

extends Node3D

const Nameplate := preload("res://scenes/world/nameplate.gd")

# Pool sized well above a realistic on-screen AoI entity count; the oldest active plate is
# recycled early only in the pathological case the pool is momentarily exhausted.
const POOL_SIZE := 32

# Distance fade/cull (metres from the camera to the plate). Fully opaque within FADE_START,
# linearly fading to transparent by CULL_DIST, hidden beyond it.
const FADE_START := 40.0
const CULL_DIST := 60.0

var _free: Array = []          # recycled, hidden plates ready to reuse
var _active: Dictionary = {}   # guid:int -> MeridianNameplate currently attached


func _ready() -> void:
	# Build the pool ONCE as hidden children (no per-spawn allocation thereafter).
	for i in range(POOL_SIZE):
		var np: Node3D = Nameplate.new()
		np.name = "Nameplate_%d" % i
		np.visible = false
		add_child(np)
		_free.push_back(np)


# --- Attach / update / recycle (world.gd -> manager) -------------------------

# Attach a nameplate over `host` (the entity's Node3D) for `guid`, seeded with the
# ENTITY_ENTER name + health. Idempotent: a second attach for a live guid just refreshes it.
# Returns the plate so callers/tests can inspect it.
func attach(guid: int, host: Node3D, entity_name: String, current_hp: int, max_hp: int) -> Node3D:
	if _active.has(guid):
		var existing: Node3D = _active[guid]
		existing.set_name_text(entity_name)
		existing.set_health(current_hp, max_hp)
		return existing
	var np: Node3D = _take()
	# Reparent from the pool onto the entity node so the plate tracks it as it moves.
	if np.get_parent() != null:
		np.get_parent().remove_child(np)
	host.add_child(np)
	np.position = Vector3.ZERO
	# Fresh attach: a recycled plate may have been an NPC last life — reset to the default
	# player/mob look (health bar shown, opaque name) until/unless a giver marker re-flags it.
	np.set_is_npc(false)
	np.set_name_text(entity_name)
	np.set_health(current_hp, max_hp)
	np.set_alpha(1.0)
	np.visible = true
	_active[guid] = np
	return np


# Update a live plate's health from a VITALS_UPDATE (no-op if the guid has no plate).
func update_vitals(guid: int, current_hp: int, max_hp: int) -> void:
	if _active.has(guid):
		_active[guid].set_health(current_hp, max_hp)


# Update a live plate's name (no-op if the guid has no plate).
func update_name(guid: int, entity_name: String) -> void:
	if _active.has(guid) and not entity_name.is_empty():
		_active[guid].set_name_text(entity_name)


# Flag a live plate as a quest/friendly NPC (#859): the server pushed it a giver marker, so
# it drops the health bar and lowers + fades its name to clear the overhead !/? glyph. No-op
# if the guid has no plate (the marker handler only calls this for a spawned remote).
func mark_npc(guid: int) -> void:
	if _active.has(guid):
		_active[guid].set_is_npc(true)


# Detach the plate for `guid` and return it to the pool (hidden). Reparents it back to the
# manager BEFORE the caller frees the entity node, so the pooled plate is never freed with it.
func recycle(guid: int) -> void:
	if not _active.has(guid):
		return
	var np: Node3D = _active[guid]
	_active.erase(guid)
	var parent := np.get_parent()
	if parent != null:
		parent.remove_child(np)
	np.visible = false
	add_child(np)
	_free.push_back(np)


# --- Introspection (verification / debug) ------------------------------------

func active_count() -> int:
	return _active.size()

func free_count() -> int:
	return _free.size()

func pool_size() -> int:
	return POOL_SIZE

func has(guid: int) -> bool:
	return _active.has(guid)

func plate_for(guid: int) -> Node3D:
	return _active.get(guid, null)


# --- Distance fade/cull (the only per-frame work) ----------------------------

func _process(_delta: float) -> void:
	if _active.is_empty() or not is_inside_tree():
		return
	var cam := get_viewport().get_camera_3d()
	if cam == null:
		return  # no camera (headless verify) — leave plates fully visible
	var cam_pos := cam.global_position
	for guid in _active:
		var np: Node3D = _active[guid]
		var d := cam_pos.distance_to(np.global_position)
		if d >= CULL_DIST:
			np.visible = false
			continue
		np.visible = true
		var a := 1.0
		if d > FADE_START:
			a = 1.0 - (d - FADE_START) / (CULL_DIST - FADE_START)
		np.set_alpha(a)


# --- Pool internals ----------------------------------------------------------

# Pull a hidden plate from the free list; when the pool is exhausted, recycle the OLDEST
# active plate so an attach never allocates and never fails.
func _take() -> Node3D:
	if _free.is_empty():
		var oldest: int = _active.keys()[0]
		recycle(oldest)
	return _free.pop_back()
