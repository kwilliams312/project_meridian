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


# =============================================================================
# QUEST + GOSSIP state (QST-01 / NPC-01/02, #371/#372/#433)
# =============================================================================
# The SAME MVVM seam as the vitals registry above: the net layer PUBLISHES decoded
# quest/gossip server events here; the quest windows / tracker / giver indicators
# SUBSCRIBE to the signals and READ through the getters — they never touch the net
# thread. SERVER IS LAW: the bus only stores + re-emits what worldd sent (a QUEST_LOG
# snapshot, a QUEST_PROGRESS delta, a GOSSIP_MENU, the typed accept/turn-in results).
# It never predicts, derives, or advances quest state.
#
# The reverse direction (a view wants to DO something — open gossip, accept a quest,
# turn one in, ask for the log) is an INTENT: the view calls a request_*() method, the
# bus re-emits it as an intent signal, and the world scene (which owns both the bus and
# the net thread) translates it to an outbound frame. So a view still knows ONLY the
# bus — the net thread stays hidden behind this one seam (extends the #431 contract).

# world.fbs GossipOptionKind ordinals (kept here so views read them by name).
const GOSSIP_QUEST_AVAILABLE := 0
const GOSSIP_QUEST_IN_PROGRESS := 1
const GOSSIP_QUEST_COMPLETE := 2
const GOSSIP_VENDOR := 3
const GOSSIP_TRAINER := 4

# --- View subscription signals (bus -> quest/gossip view-models) -------------

## A GOSSIP_MENU arrived for `npc_guid`. `options` is an Array of option Dictionaries
## {kind:int (GossipOptionKind), target_id:int (quest id for quest kinds, else 0)}.
## The gossip window opens/rebinds to it.
signal gossip_menu_changed(npc_guid: int, options: Array)

## Gossip was dismissed (the player closed the window / talked to no one). Views hide.
signal gossip_closed()

## The active quest log snapshot changed (a QUEST_LOG or an accept/turn-in resync).
## `quests` is an Array of quest Dictionaries (see _QUEST_ENTRY_KEYS). The quest log
## window + tracker re-render from it.
signal quest_log_changed(quests: Array)

## A single objective advanced (QUEST_PROGRESS). The record is already merged into the
## log; this fires so the tracker can flash the specific line without a full rebuild.
signal quest_progress_changed(quest_id: int, objective_index: int, have: int, need: int, complete: bool)

## A typed QUEST_ACCEPT_RESULT (status is world.fbs QuestAcceptStatus; 0 = OK).
signal quest_accept_result(quest_id: int, status: int)

## A typed QUEST_TURN_IN_RESULT. `result` carries {quest_id, status, reward_xp,
## reward_money, new_level, reward_items:[{item_id,count}]} (status 0 = OK).
signal quest_turn_in_result(result: Dictionary)

## The tracked (watched) quest changed. `quest_id` == 0 means nothing is tracked.
signal tracked_quest_changed(quest_id: int)

## A quest-giver NPC's indicator changed. `marker` is "!" (a quest is available here),
## "?" (a quest is turn-in-ready here), or "" (no marker). The world scene floats it
## over the NPC when one exists.
signal giver_indicator_changed(npc_guid: int, marker: String)

# --- Intent signals (view -> the world scene, which owns the net thread) ------

signal gossip_hello_requested(npc_guid: int)          ## open gossip on an NPC
signal quest_accept_requested(quest_id: int, giver_guid: int)
signal quest_turn_in_requested(quest_id: int, turn_in_guid: int, choice_index: int)
signal quest_log_requested()                          ## ask worldd for the log snapshot

## Gossip ENTRY hooks for the sibling vendor / trainer windows (#440). The gossip
## window emits these when the player picks a VENDOR / TRAINER menu row; the #440
## windows subscribe and open themselves. This story only wires the entry points.
signal vendor_entry_selected(npc_guid: int)
signal trainer_entry_selected(npc_guid: int)

# --- Quest/gossip registry state ---------------------------------------------
# Canonical shape of one quest-log entry the bus stores + emits.
const _QUEST_ENTRY_KEYS := ["quest_id", "level", "complete", "objectives"]

var _quests: Dictionary = {}          # quest_id:int -> quest entry Dictionary
var _quest_order: Array = []          # quest_ids in server order (stable render order)
var _tracked_quest: int = 0           # the watched quest (0 = none)
var _gossip_npc: int = 0              # the NPC whose gossip menu is open (0 = none)
var _gossip_options: Array = []       # the open menu's options (empty = closed)
var _giver_markers: Dictionary = {}   # npc_guid:int -> "!"/"?"/"" (last computed)

# --- Publish API (the net layer -> bus) --------------------------------------

## Publish a GOSSIP_MENU (decode_quest_frame kind "gossip_menu"). Stores the open menu,
## recomputes the NPC's giver indicator from the option kinds, and emits.
func publish_gossip_menu(npc_guid: int, options: Array) -> void:
	_gossip_npc = npc_guid
	_gossip_options = options.duplicate(true)
	gossip_menu_changed.emit(npc_guid, _gossip_options)
	_recompute_giver_marker(npc_guid, options)


## Publish a QUEST_ACCEPT_RESULT. On OK, watch the newly-accepted quest so the tracker
## follows it immediately (a QUEST_LOG resync with the objectives follows from worldd).
func publish_quest_accept_result(quest_id: int, status: int) -> void:
	quest_accept_result.emit(quest_id, status)
	if status == 0:  # QuestAcceptStatus.OK
		set_tracked_quest(quest_id)


## Publish a QUEST_PROGRESS delta. Merges `have/need/complete` onto the tracked log
## entry's objective (so the tracker reads a consistent snapshot) and emits.
func publish_quest_progress(data: Dictionary) -> void:
	var quest_id := int(data.get("quest_id", 0))
	var idx := int(data.get("objective_index", 0))
	var have := int(data.get("have", 0))
	var need := int(data.get("need", 0))
	var complete := bool(data.get("complete", false))
	if _quests.has(quest_id):
		var entry: Dictionary = _quests[quest_id]
		var objs: Array = entry.get("objectives", [])
		if idx >= 0 and idx < objs.size():
			var o: Dictionary = objs[idx]
			o["have"] = have
			o["need"] = need
			o["complete"] = complete
			objs[idx] = o
		# Re-derive whether every objective is now complete (turn-in readiness).
		entry["complete"] = _all_objectives_complete(objs)
		_quests[quest_id] = entry
	quest_progress_changed.emit(quest_id, idx, have, need, complete)


## Publish a QUEST_TURN_IN_RESULT. On OK the quest completed — drop it from the log and
## re-pick a tracked quest (a QUEST_LOG resync follows from worldd regardless).
func publish_quest_turn_in_result(result: Dictionary) -> void:
	quest_turn_in_result.emit(result.duplicate(true))
	if int(result.get("status", -1)) == 0:  # QuestTurnInStatus.OK
		var quest_id := int(result.get("quest_id", 0))
		if _quests.has(quest_id):
			_quests.erase(quest_id)
			_quest_order.erase(quest_id)
			if _tracked_quest == quest_id:
				set_tracked_quest(_first_active_quest())
			quest_log_changed.emit(quest_log())


## Publish a QUEST_LOG snapshot (decode_quest_frame kind "quest_log"). Replaces the
## whole registry with the server's authoritative active-quest list and emits. If the
## tracked quest is gone (or none was tracked), tracks the first active quest.
func publish_quest_log(quests: Array) -> void:
	_quests.clear()
	_quest_order.clear()
	for q in quests:
		var entry := _quest_entry_from(q)
		var qid := int(entry["quest_id"])
		_quests[qid] = entry
		_quest_order.append(qid)
	if not _quests.has(_tracked_quest):
		set_tracked_quest(_first_active_quest())
	quest_log_changed.emit(quest_log())

# --- Request API (view -> intent signal; the world scene sends the frame) -----

func request_gossip_hello(npc_guid: int) -> void:
	gossip_hello_requested.emit(npc_guid)


func request_quest_accept(quest_id: int, giver_guid: int) -> void:
	quest_accept_requested.emit(quest_id, giver_guid)


func request_quest_turn_in(quest_id: int, turn_in_guid: int, choice_index: int = -1) -> void:
	quest_turn_in_requested.emit(quest_id, turn_in_guid, choice_index)


func request_quest_log() -> void:
	quest_log_requested.emit()


## Gossip entry hooks (#440): the gossip window calls these when a vendor / trainer row
## is picked. Re-emitted for the sibling windows to open (no server frame here).
func request_vendor_entry(npc_guid: int) -> void:
	vendor_entry_selected.emit(npc_guid)


func request_trainer_entry(npc_guid: int) -> void:
	trainer_entry_selected.emit(npc_guid)


## Dismiss the open gossip menu (a UI-only action — no server frame).
func close_gossip() -> void:
	_gossip_npc = 0
	_gossip_options = []
	gossip_closed.emit()


## Watch `quest_id` in the tracker (0 = none). Idempotent.
func set_tracked_quest(quest_id: int) -> void:
	if quest_id == _tracked_quest:
		return
	_tracked_quest = quest_id
	tracked_quest_changed.emit(quest_id)

# --- Read API (bus -> view-models; all return copies) ------------------------

## The active quest log as an Array of entry copies, in server order.
func quest_log() -> Array:
	var out: Array = []
	for qid in _quest_order:
		out.append((_quests[qid] as Dictionary).duplicate(true))
	return out


## True iff `quest_id` is in the active log.
func has_quest(quest_id: int) -> bool:
	return _quests.has(quest_id)


## The watched quest id (0 = none).
func tracked_quest() -> int:
	return _tracked_quest


## The tracked quest's entry (empty Dictionary if nothing tracked / not in the log).
func tracked_quest_entry() -> Dictionary:
	if _tracked_quest != 0 and _quests.has(_tracked_quest):
		return (_quests[_tracked_quest] as Dictionary).duplicate(true)
	return {}


## The NPC whose gossip menu is currently open (0 = none).
func gossip_npc() -> int:
	return _gossip_npc


## The open gossip menu's options (empty when closed).
func gossip_options() -> Array:
	return _gossip_options.duplicate(true)


## The last computed giver marker for `npc_guid` ("!"/"?"/"").
func giver_marker(npc_guid: int) -> String:
	return String(_giver_markers.get(npc_guid, ""))

# --- Quest/gossip internals --------------------------------------------------

# Normalize a raw decode_quest_frame quest Dictionary into the canonical entry shape.
func _quest_entry_from(q: Dictionary) -> Dictionary:
	var objectives: Array = []
	for o in q.get("objectives", []):
		objectives.append({
			"type": int(o.get("type", 0)),
			"target_id": int(o.get("target_id", 0)),
			"have": int(o.get("have", 0)),
			"need": int(o.get("need", 0)),
			"complete": bool(o.get("complete", false)),
		})
	return {
		"quest_id": int(q.get("quest_id", 0)),
		"level": int(q.get("level", 0)),
		"complete": bool(q.get("complete", false)),
		"objectives": objectives,
	}


static func _all_objectives_complete(objs: Array) -> bool:
	if objs.is_empty():
		return false
	for o in objs:
		if not bool((o as Dictionary).get("complete", false)):
			return false
	return true


# The first active quest in server order (0 if the log is empty). Used to auto-pick a
# tracked quest when the current one is turned in / absent.
func _first_active_quest() -> int:
	return int(_quest_order[0]) if not _quest_order.is_empty() else 0


# Derive an NPC's giver indicator from a gossip menu's option kinds and emit it:
# a turn-in-ready quest ("?") outranks an available one ("!"); otherwise no marker.
func _recompute_giver_marker(npc_guid: int, options: Array) -> void:
	var has_available := false
	var has_complete := false
	for o in options:
		match int((o as Dictionary).get("kind", -1)):
			GOSSIP_QUEST_COMPLETE: has_complete = true
			GOSSIP_QUEST_AVAILABLE: has_available = true
	var marker := "?" if has_complete else ("!" if has_available else "")
	if String(_giver_markers.get(npc_guid, "")) == marker:
		return
	_giver_markers[npc_guid] = marker
	giver_indicator_changed.emit(npc_guid, marker)
