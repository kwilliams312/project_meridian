# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the client MVVM EVENT BUS / server-state registry (client SAD
# §2.7; UI-01, #431). This is the FOUNDATION the whole HUD hangs on, and the future
# UI-02 addon surface (epic #24), so it is built as a clean, documented seam from
# day one.
#
# ONE-WAY DATA FLOW (Model-View-ViewModel):
#
#     worldd  ──wire──▶  net layer (MeridianNetThread)  ──signal──▶  world.gd
#                                                                       │ publish_*()
#                                                                       ▼
#                                                             ┌───────────────────┐
#                                                             │  MeridianEventBus │  ← the ONE registry
#                                                             │  (this file)      │
#                                                             └───────────────────┘
#                                                                       │ signals
#                                              ┌────────────────────────┼────────────────────────┐
#                                              ▼                        ▼                        ▼
#                                       player unit frame        target unit frame        (future UI-02 addons)
#
# The net layer PUBLISHES decoded server events here; UI view-models SUBSCRIBE to the
# signals and READ state through the getters. UI code NEVER touches the net thread
# directly — the bus is the single source of truth for server state, so a view and a
# future addon see byte-identical data and can never drift.
#
# SERVER IS LAW (Principle 1): every field here is server-authoritative. The bus only
# STORES and RE-EMITS what the server sent (EntityEnter vitals + VITALS_UPDATE deltas,
# #430). It never predicts, derives, or mutates gameplay state.
#
# PERFORMANCE (epic exit criterion ≤2 ms GDScript combat budget, #431): the bus is
# 100% EVENT-DRIVEN. It has no _process/_physics_process; it does work ONLY when the
# net layer publishes an event (a handful of times per second, not per frame). Views
# likewise update on these signals, never by polling — so the HUD adds no per-frame
# cost and no per-frame allocation.
#
# A RefCounted (not a Node / not an autoload): it is created and owned by the world
# scene and injected into the HUD. This keeps it trivially unit-testable headlessly
# (no SceneTree, no display, no server — see hud/hud_verify.gd) and scoped to a world
# session. Promoting it to a project autoload for cross-scene addon reach is a UI-02
# concern; the API below is already the stable seam that promotion would preserve.
class_name MeridianEventBus
extends RefCounted

# --- Signals (the ViewModel subscription surface) ----------------------------

## An entity's vitals were (re)published — a full EntityEnter snapshot or a
## VITALS_UPDATE delta. `vitals` is the merged, canonical record (see _EMPTY_VITALS
## for the shape). Views bound to this guid re-render from it.
signal entity_vitals_changed(guid: int, vitals: Dictionary)

## An entity left the client's area of interest (EntityLeave). Views drop it.
signal entity_removed(guid: int)

## The LOCAL player's entity guid was identified (or changed). The player unit frame
## rebinds to it. `guid` == 0 means "not yet known".
signal local_player_changed(guid: int)

## The current TARGET changed (target select / clear). `guid` == 0 means no target.
signal target_changed(guid: int)

# --- Canonical vitals record shape -------------------------------------------
# Every stored/emitted vitals Dictionary carries exactly these keys, so a view can
# read them without has()-guards. Ints throughout; `name` is a String.
const _EMPTY_VITALS := {
	"name": "",
	"level": 0,
	"health": 0,
	"max_health": 0,
	"power": 0,
	"max_power": 0,
	"power_type": 0,   # MeridianPowerColors.NONE
	"char_class": 0,   # roster class id (#328) — colors nameplates later
}

# --- Registry state ----------------------------------------------------------
var _entities: Dictionary = {}   # guid:int -> vitals Dictionary (canonical shape)
var _local_guid: int = 0
var _target_guid: int = 0

# --- Publish API (the net layer -> bus) --------------------------------------
# world.gd calls these from its server-event signal handlers. Each stores the new
# state and emits the matching signal so subscribed views react.

## Publish a full EntityEnter snapshot (guid + name + level + vitals + class). This
## is the authoritative baseline for a unit's frame — it carries the `name` a later
## VITALS_UPDATE (which has no name) will not. `data` is the Dictionary from
## MeridianNetThread.decode_entity_frame() with kind == "enter".
func publish_entity_enter(guid: int, data: Dictionary) -> void:
	if guid == 0:
		return
	var v := _record_for(guid)
	v["name"] = String(data.get("name", v["name"]))
	v["level"] = int(data.get("level", v["level"]))
	v["char_class"] = int(data.get("char_class", v["char_class"]))
	_apply_vitals(v, data)
	_entities[guid] = v
	entity_vitals_changed.emit(guid, v)

## Publish a VITALS_UPDATE delta (health/power/level; NO name — it does not change
## in-life). Merges onto the existing record so the EntityEnter `name`/`char_class`
## survive. If the entity is unknown (e.g. the local player, which gets no self
## EntityEnter at spawn), a partial record is created so the frame still populates.
func publish_vitals_update(guid: int, data: Dictionary) -> void:
	if guid == 0:
		return
	var v := _record_for(guid)
	_apply_vitals(v, data)
	if int(data.get("level", 0)) > 0:
		v["level"] = int(data["level"])
	_entities[guid] = v
	entity_vitals_changed.emit(guid, v)

## Seed a locally-known identity (name/level/class from character-select) for a guid
## that the server will not send a self EntityEnter for — the LOCAL player. Vitals
## (health/power) stay 0 until the first self VITALS_UPDATE. Idempotent: it never
## clobbers a name/level already learned from the wire with an empty/zero seed.
func seed_identity(guid: int, name: String, level: int, char_class: int) -> void:
	if guid == 0:
		return
	var v := _record_for(guid)
	if not name.is_empty():
		v["name"] = name
	if level > 0 and int(v["level"]) == 0:
		v["level"] = level
	if char_class > 0:
		v["char_class"] = char_class
	_entities[guid] = v
	entity_vitals_changed.emit(guid, v)

## Publish an EntityLeave — the entity left AoI. Drops its record and, if it was the
## current target, clears the target so the target frame hides.
func publish_entity_leave(guid: int) -> void:
	if not _entities.has(guid):
		return
	_entities.erase(guid)
	entity_removed.emit(guid)
	if guid == _target_guid:
		set_target(0)

## Identify the LOCAL player's entity guid (learned from the authoritative
## MovementState). The player unit frame binds to it.
func set_local_player(guid: int) -> void:
	if guid == _local_guid:
		return
	_local_guid = guid
	local_player_changed.emit(guid)

## Set (or clear, with 0) the current target. The target unit frame binds to it.
func set_target(guid: int) -> void:
	if guid == _target_guid:
		return
	_target_guid = guid
	target_changed.emit(guid)

# --- Read API (the bus -> UI view-models) ------------------------------------
# The ONLY place UI reads server state from. All return by VALUE (a copy), so a view
# can never mutate the registry.

## The current local-player guid (0 = not yet known).
func local_guid() -> int:
	return _local_guid

## The current target guid (0 = none).
func target_guid() -> int:
	return _target_guid

## True iff a vitals record exists for `guid`.
func has_entity(guid: int) -> bool:
	return _entities.has(guid)

## The canonical vitals record for `guid` (a copy). Returns an all-zero record (the
## _EMPTY_VITALS shape) when the entity is unknown, so callers never has()-guard keys.
func get_vitals(guid: int) -> Dictionary:
	if _entities.has(guid):
		return (_entities[guid] as Dictionary).duplicate()
	return _EMPTY_VITALS.duplicate()

## The local player's vitals (empty record if the local guid is unknown/absent).
func local_vitals() -> Dictionary:
	return get_vitals(_local_guid) if _local_guid != 0 else _EMPTY_VITALS.duplicate()

## The current target's vitals (empty record if there is no target).
func target_vitals() -> Dictionary:
	return get_vitals(_target_guid) if _target_guid != 0 else _EMPTY_VITALS.duplicate()

# --- Internals ---------------------------------------------------------------

# Fetch a mutable copy of guid's record, or a fresh canonical-shape record.
func _record_for(guid: int) -> Dictionary:
	if _entities.has(guid):
		return (_entities[guid] as Dictionary).duplicate()
	return _EMPTY_VITALS.duplicate()

# Copy the vitals scalars present in `data` onto `v` (health/power block). `name`,
# `level` and `char_class` are handled by the callers (they differ enter vs delta).
static func _apply_vitals(v: Dictionary, data: Dictionary) -> void:
	v["health"] = int(data.get("health", v["health"]))
	v["max_health"] = int(data.get("max_health", v["max_health"]))
	v["power"] = int(data.get("power", v["power"]))
	v["max_power"] = int(data.get("max_power", v["max_power"]))
	v["power_type"] = int(data.get("power_type", v["power_type"]))
