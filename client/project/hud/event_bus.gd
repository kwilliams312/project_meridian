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
# Canonical shape of one quest-log entry the bus stores + emits — including the REWARD
# PREVIEW (#443): flat XP + copper, the always-granted `reward_items`, and the one-of
# `choice_items` the turn-in dialog renders its picker from (empty = no picker).
const _QUEST_ENTRY_KEYS := ["quest_id", "level", "complete", "objectives",
	"reward_xp", "reward_money", "reward_items", "choice_items"]

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


## A specific quest's entry (a copy) including its reward preview, or an empty Dictionary
## if `quest_id` is not in the active log. The turn-in dialog reads the reward preview +
## choice_items from here to render the picker (#443/#442).
func quest_entry(quest_id: int) -> Dictionary:
	if _quests.has(quest_id):
		return (_quests[quest_id] as Dictionary).duplicate(true)
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

# Normalize a raw decode_quest_frame quest Dictionary into the canonical entry shape,
# including the reward preview (flat XP/copper + always-granted items + one-of choices).
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
		"reward_xp": int(q.get("reward_xp", 0)),
		"reward_money": int(q.get("reward_money", 0)),
		"reward_items": _reward_items_from(q.get("reward_items", [])),
		"choice_items": _reward_items_from(q.get("choice_items", [])),
	}


# Normalize a raw reward/choice item Array into canonical {item_id, count} entries.
static func _reward_items_from(items: Array) -> Array:
	var out: Array = []
	for it in items:
		out.append({
			"item_id": int((it as Dictionary).get("item_id", 0)),
			"count": int((it as Dictionary).get("count", 1)),
		})
	return out


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


# =============================================================================
# LOOT + VENDOR + TRAINER + CURRENCY state (ITM-01/02, ECO-01, NPC-02; #369/#370/#372/#441)
# =============================================================================
# The SAME MVVM seam as the vitals/quest registries above: the net layer PUBLISHES
# decoded loot/vendor/trainer server events here; the loot / vendor / trainer / bags
# windows SUBSCRIBE to the signals and READ through the getters — they never touch the
# net thread. SERVER IS LAW: the bus only stores + re-emits what worldd sent (a
# LOOT_RESPONSE, a typed take/buy/sell/buyback/learn result, a pushed TRAINER_LIST). It
# never rolls loot, prices an item, decides learn eligibility, or predicts a balance.
#
# The reverse direction (a view wants to DO something — open a corpse, take a slot,
# buy / sell / buyback, learn) is an INTENT: the view calls a request_*() method, the bus
# re-emits it as an intent signal, and the world scene turns it into an outbound frame
# (extends the #431/#433 contract — the net thread stays hidden behind this one seam).
#
# WIRE CONTRACTS (both formerly-greybox gaps are now CLOSED by #453/#471):
#   1. INVENTORY_SNAPSHOT (0x5007) — worldd pushes the character's item list + money to the
#      owning client at ENTER_WORLD and after every server-authoritative inventory change,
#      so the bags window renders the REAL items (slot/template/count/quality) + balance and
#      the money is known from spawn (no longer "unknown until a transaction").
#   2. VENDOR_LIST (0x5107) — auto-pushed on GOSSIP_HELLO to a vendor NPC (like TRAINER_LIST),
#      so the vendor BUY tab renders the server-computed catalog (item + price + stock).
# The BUYBACK tab is likewise fully wire-populated (from each VENDOR_SELL_RESULT), and the
# VENDOR_BUYBACK_RESULT now ECHOES its buyback_slot so the client drops the repurchased row
# without self-correlating against its outstanding request.

# world.fbs enum ordinals kept here so views read them by name (mirrors the gossip block).
# LootStatus (LOOT_RESPONSE) / LootTakeStatus (LOOT_RESULT).
const LOOT_OK := 0
# VendorBuyStatus / VendorSellStatus / VendorBuybackStatus / TrainerLearnStatus: 0 = OK.
const RESULT_OK := 0
# TrainableState (a TRAINER_LIST row's learn eligibility).
const TRAIN_LEARNABLE := 0
const TRAIN_ALREADY_KNOWN := 1
const TRAIN_WRONG_CLASS := 2
const TRAIN_LEVEL_TOO_LOW := 3
const TRAIN_CANT_AFFORD := 4

# --- View subscription signals (bus -> loot/vendor/trainer/bags view-models) --

## A LOOT_RESPONSE arrived for `corpse_guid`: `copper` money pile + `items` (Array of
## {slot,item_template_id,count,quality,quest_item}). The loot window opens/rebinds.
signal loot_window_changed(corpse_guid: int, status: int, copper: int, items: Array)

## A LOOT_RESULT (a take outcome) — already merged into the loot window state; fires so a
## view can flash the taken row / surface a typed rejection. `result` carries
## {corpse_guid,slot,status,item_template_id,count,copper}.
signal loot_result(result: Dictionary)

## The loot window closed (LOOT_RELEASE echo / corpse looted-out). Views hide.
signal loot_closed(corpse_guid: int)

## The vendor window should open on `vendor_id` (the gossip vendor entry was picked).
signal vendor_opened(vendor_id: int)

## A vendor transaction result. `result` carries a "kind" ("buy"/"sell"/"buyback"), the
## typed `status`, and the transaction fields. Already merged (balance + buyback queue).
signal vendor_result(result: Dictionary)

## The buyback queue changed (a sell pushed an entry / a buyback removed one). `entries`
## is an Array of {buyback_slot,item_template_id,quantity,price} in queue order.
signal vendor_buyback_changed(entries: Array)

## A TRAINER_LIST arrived (pushed alongside GOSSIP_MENU for a trainer NPC). `entries` is
## an Array of {ability_id,cost,required_class,required_level,state}. The trainer window
## rebinds; it opens when the gossip trainer entry is picked (trainer_opened).
signal trainer_list_changed(npc_guid: int, entries: Array)

## The trainer window should open on `npc_guid` (the gossip trainer entry was picked).
signal trainer_opened(npc_guid: int)

## A TRAINER_LEARN_RESULT. `result` carries {npc_guid,ability_id,status,cost,new_balance}.
## Already merged (balance + known-ability set).
signal trainer_learn_result(result: Dictionary)

## The server-authoritative copper balance changed (from an INVENTORY_SNAPSHOT `money` or a
## transaction result's balance / new_balance). `known` is false only before the FIRST such
## message; an INVENTORY_SNAPSHOT rides at ENTER_WORLD, so money is known from spawn.
signal currency_changed(copper: int, known: bool)

## An INVENTORY_SNAPSHOT arrived (decode_econ_frame kind "inventory_snapshot"): the bags
## window's real contents. `items` is an Array of {slot,item_template_id,count,quality,
## binding}; `backpack_slots` is the grid capacity. The bags window re-renders.
signal inventory_changed(money: int, items: Array, backpack_slots: int)

## A VENDOR_LIST arrived (decode_econ_frame kind "vendor_list"): the vendor's for-sale
## catalog. `items` is an Array of {item_template_id,price,quality,stock}. The vendor
## window's BUY tab re-renders with the server-computed prices.
signal vendor_catalog_changed(vendor_id: int, items: Array)

# --- Intent signals (view -> the world scene, which owns the net thread) ------

signal loot_request_requested(corpse_guid: int)
signal loot_take_requested(corpse_guid: int, slot: int, money: bool)
signal loot_release_requested(corpse_guid: int)
signal vendor_buy_requested(vendor_id: int, item_template_id: int, quantity: int)
signal vendor_sell_requested(vendor_id: int, backpack_slot: int, quantity: int)
signal vendor_buyback_requested(buyback_slot: int)
signal trainer_learn_requested(npc_guid: int, ability_id: int)

# --- Loot/vendor/trainer/currency registry state -----------------------------
var _loot_corpse: int = 0            # the corpse whose loot window is open (0 = none)
var _loot_copper: int = 0            # money still on the open corpse
var _loot_items: Array = []          # remaining lootable slots on the open corpse
var _vendor_id: int = 0              # the open vendor (0 = none)
var _vendor_catalog: Array = []      # the open vendor's VENDOR_LIST rows: {item_template_id,price,quality,stock}
var _inventory: Array = []           # backpack contents (INVENTORY_SNAPSHOT): {slot,item_template_id,count,quality,binding}
var _backpack_slots: int = 0         # backpack grid capacity (cell count)
var _inventory_known: bool = false   # an INVENTORY_SNAPSHOT has been seen
var _buyback: Array = []             # buyback queue: {buyback_slot,item_template_id,quantity,price}
var _trainer_npc: int = 0            # the trainer whose list we hold (0 = none)
var _trainer_entries: Array = []     # the held TRAINER_LIST rows
var _learned: Dictionary = {}        # ability_id -> true (learned this session, for greying)
var _copper: int = 0                 # server-authoritative balance (valid iff _copper_known)
var _copper_known: bool = false      # a balance-carrying result has been seen

# --- Publish API (the net layer -> bus) --------------------------------------

## Publish a LOOT_RESPONSE (decode_econ_frame kind "loot_response"). Stores the open
## corpse's money + slots and emits. A non-OK status carries no items (a pre-check failed).
func publish_loot_response(corpse_guid: int, status: int, copper: int, items: Array) -> void:
	_loot_corpse = corpse_guid
	_loot_copper = copper if status == LOOT_OK else 0
	_loot_items = items.duplicate(true) if status == LOOT_OK else []
	loot_window_changed.emit(corpse_guid, status, _loot_copper, _loot_items)


## Publish a LOOT_RESULT (kind "loot_result"). On an OK take, removes the taken slot (or
## clears the money pile) from the open corpse so the window reflects what remains, then
## emits. A non-OK status leaves the corpse unchanged (the reason rides in `result`).
func publish_loot_result(result: Dictionary) -> void:
	var status := int(result.get("status", -1))
	if status == LOOT_OK and int(result.get("corpse_guid", 0)) == _loot_corpse:
		if int(result.get("copper", 0)) > 0 and int(result.get("item_template_id", 0)) == 0:
			_loot_copper = 0  # the money pile was taken
		else:
			var slot := int(result.get("slot", -1))
			var kept: Array = []
			for it in _loot_items:
				if int((it as Dictionary).get("slot", -1)) != slot:
					kept.append(it)
			_loot_items = kept
		loot_window_changed.emit(_loot_corpse, LOOT_OK, _loot_copper, _loot_items)
	loot_result.emit(result.duplicate(true))


## Publish a LOOT_CLOSED (kind "loot_closed"). Clears the open corpse + emits.
func publish_loot_closed(corpse_guid: int) -> void:
	if corpse_guid == _loot_corpse:
		_loot_corpse = 0
		_loot_copper = 0
		_loot_items = []
	loot_closed.emit(corpse_guid)


## Publish an INVENTORY_SNAPSHOT (decode_econ_frame kind "inventory_snapshot"). Replaces the
## held backpack contents + capacity with the server's, applies the authoritative `money`
## balance (so currency is known from spawn), and emits inventory_changed. SERVER IS LAW —
## the client never predicts its bags; it renders exactly what worldd re-sends.
func publish_inventory_snapshot(money: int, items: Array, backpack_slots: int) -> void:
	_inventory = items.duplicate(true)
	_backpack_slots = backpack_slots
	_inventory_known = true
	_apply_balance(money)
	inventory_changed.emit(money, inventory_items(), _backpack_slots)


## Publish a VENDOR_LIST (decode_econ_frame kind "vendor_list"). Stores the open vendor's
## server-computed catalog + emits vendor_catalog_changed. Auto-pushed on GOSSIP_HELLO to a
## vendor NPC; a pure DISPLAY projection (the buy path re-validates every price server-side).
func publish_vendor_list(vendor_id: int, items: Array) -> void:
	_vendor_id = vendor_id
	_vendor_catalog = items.duplicate(true)
	vendor_catalog_changed.emit(vendor_id, vendor_catalog())


## Publish a VENDOR_BUY_RESULT. Applies the server balance, emits the typed result.
func publish_vendor_buy_result(result: Dictionary) -> void:
	_apply_balance(int(result.get("balance", 0)))
	var out := result.duplicate(true)
	out["kind"] = "buy"
	vendor_result.emit(out)


## Publish a VENDOR_SELL_RESULT. On OK, applies the balance and pushes the sold stack onto
## the buyback queue at its server-assigned slot; emits the typed result + the queue.
func publish_vendor_sell_result(result: Dictionary) -> void:
	_apply_balance(int(result.get("balance", 0)))
	if int(result.get("status", -1)) == RESULT_OK:
		var slot := int(result.get("buyback_slot", 0))
		var entry := {
			"buyback_slot": slot,
			"item_template_id": int(result.get("item_template_id", 0)),
			"quantity": int(result.get("quantity", 0)),
			"price": int(result.get("total_credit", 0)),
		}
		# The server owns the slot index; replace any stale entry at that slot.
		var replaced := false
		for i in range(_buyback.size()):
			if int((_buyback[i] as Dictionary).get("buyback_slot", -1)) == slot:
				_buyback[i] = entry
				replaced = true
				break
		if not replaced:
			_buyback.append(entry)
		vendor_buyback_changed.emit(buyback_entries())
	var out := result.duplicate(true)
	out["kind"] = "sell"
	vendor_result.emit(out)


## Publish a VENDOR_BUYBACK_RESULT. On OK, applies the balance and removes the repurchased
## entry from the buyback queue; emits the typed result + the queue. The queue slot comes
## from the result's ECHOED `buyback_slot` (#453/#471) — the server tells us exactly which
## row it repurchased, so the client no longer self-correlates against its own request. The
## optional `fallback_slot` covers a pre-#453 server that did not echo (defaults to none).
func publish_vendor_buyback_result(result: Dictionary, fallback_slot: int = -1) -> void:
	_apply_balance(int(result.get("balance", 0)))
	var slot := int(result.get("buyback_slot", fallback_slot))
	if int(result.get("status", -1)) == RESULT_OK and slot >= 0:
		var kept: Array = []
		for e in _buyback:
			if int((e as Dictionary).get("buyback_slot", -1)) != slot:
				kept.append(e)
		_buyback = kept
		vendor_buyback_changed.emit(buyback_entries())
	var out := result.duplicate(true)
	out["kind"] = "buyback"
	vendor_result.emit(out)


## Publish a TRAINER_LIST (kind "trainer_list"). Stores the held rows + emits. Rows the
## player has already learned this session are shown as known regardless of the wire state.
func publish_trainer_list(npc_guid: int, entries: Array) -> void:
	_trainer_npc = npc_guid
	_trainer_entries = entries.duplicate(true)
	trainer_list_changed.emit(npc_guid, trainer_entries())


## Publish a TRAINER_LEARN_RESULT. On OK, applies new_balance and marks the ability known
## (so the row greys out immediately); emits the typed result.
func publish_trainer_learn_result(result: Dictionary) -> void:
	_apply_balance(int(result.get("new_balance", 0)))
	if int(result.get("status", -1)) == RESULT_OK:
		_learned[int(result.get("ability_id", 0))] = true
		# Re-emit the list so a bound trainer window regreys the learned row.
		if _trainer_npc != 0:
			trainer_list_changed.emit(_trainer_npc, trainer_entries())
	trainer_learn_result.emit(result.duplicate(true))

# --- Request API (view -> intent signal; the world scene sends the frame) -----

func request_loot(corpse_guid: int) -> void:
	loot_request_requested.emit(corpse_guid)


func request_loot_take(corpse_guid: int, slot: int, money: bool = false) -> void:
	loot_take_requested.emit(corpse_guid, slot, money)


func request_loot_release(corpse_guid: int) -> void:
	loot_release_requested.emit(corpse_guid)


func request_vendor_buy(vendor_id: int, item_template_id: int, quantity: int) -> void:
	vendor_buy_requested.emit(vendor_id, item_template_id, quantity)


func request_vendor_sell(vendor_id: int, backpack_slot: int, quantity: int) -> void:
	vendor_sell_requested.emit(vendor_id, backpack_slot, quantity)


func request_vendor_buyback(buyback_slot: int) -> void:
	vendor_buyback_requested.emit(buyback_slot)


func request_trainer_learn(npc_guid: int, ability_id: int) -> void:
	trainer_learn_requested.emit(npc_guid, ability_id)


## Open the vendor window on `vendor_id` (called by the world scene when the gossip vendor
## entry fires). UI-only — no server frame (the wire has no vendor-catalog opcode; gap).
func open_vendor(vendor_id: int) -> void:
	_vendor_id = vendor_id
	vendor_opened.emit(vendor_id)


## Open the trainer window on `npc_guid` (called when the gossip trainer entry fires). The
## TRAINER_LIST was already pushed with the GOSSIP_MENU — this only reveals the window.
func open_trainer(npc_guid: int) -> void:
	trainer_opened.emit(npc_guid)

# --- Read API (bus -> view-models; all return copies) ------------------------

## The corpse whose loot window is open (0 = none).
func loot_corpse() -> int:
	return _loot_corpse


## The money still on the open corpse.
func loot_copper() -> int:
	return _loot_copper


## The remaining lootable slots on the open corpse (copies).
func loot_items() -> Array:
	return _loot_items.duplicate(true)


## The open vendor id (0 = none).
func vendor_id() -> int:
	return _vendor_id


## The open vendor's server-computed catalog (copies): {item_template_id,price,quality,stock}.
func vendor_catalog() -> Array:
	return _vendor_catalog.duplicate(true)


## The backpack contents from the last INVENTORY_SNAPSHOT (copies), in wire order:
## {slot,item_template_id,count,quality,binding}.
func inventory_items() -> Array:
	return _inventory.duplicate(true)


## The backpack grid capacity (cell count) from the last INVENTORY_SNAPSHOT.
func backpack_slots() -> int:
	return _backpack_slots


## True once an INVENTORY_SNAPSHOT has been seen (bags contents are authoritative).
func inventory_known() -> bool:
	return _inventory_known


## The buyback queue (copies), in queue order.
func buyback_entries() -> Array:
	return _buyback.duplicate(true)


## The held trainer's NPC guid (0 = none).
func trainer_npc() -> int:
	return _trainer_npc


## The held TRAINER_LIST rows (copies), each with a derived "known" flag folding in
## abilities learned this session (so a just-learned row greys even before a list resync).
func trainer_entries() -> Array:
	var out: Array = []
	for e in _trainer_entries:
		var row: Dictionary = (e as Dictionary).duplicate(true)
		row["known"] = bool(_learned.get(int(row.get("ability_id", 0)), false)) \
			or int(row.get("state", TRAIN_LEARNABLE)) == TRAIN_ALREADY_KNOWN
		out.append(row)
	return out


## The server-authoritative copper balance (valid only when currency_known() is true).
func copper() -> int:
	return _copper


## True once a balance-carrying result has been seen (else the balance is unknown — there
## is no spawn-time money snapshot on the wire).
func currency_known() -> bool:
	return _copper_known

# --- Loot/vendor/trainer internals -------------------------------------------

# Apply a server-authoritative absolute balance and emit currency_changed if it changed
# (or if this is the first balance we have seen). Deltas (loot money, quest rewards) are
# NOT folded in here — only absolute post-transaction balances the server states.
func _apply_balance(balance: int) -> void:
	if _copper_known and balance == _copper:
		return
	_copper = balance
	_copper_known = true
	currency_changed.emit(_copper, true)


# =============================================================================
# COMBAT: ability set + ability cast + GCD/cast sweep (CMB-01/UI-01, D-10, #432)
# =============================================================================
# The SAME MVVM seam as the registries above, applied to combat's D-10 optimistic-GCD
# path (server SAD §3.3, client SAD §2.2/§3c). The action bar / cast bar SUBSCRIBE to
# the signals + READ through the getters; a slot press is an INTENT (request_cast) the
# world scene turns into a CAST_REQUEST frame. The net layer PUBLISHES the server's typed
# replies (CAST_START accept / CAST_FAILED reject / CAST_RESULT resolution).
#
# SERVER IS LAW (Principle 1) with ONE deliberate, bounded prediction: the client starts
# the GCD OPTIMISTICALLY on the press (so the sweep feels instant) and ROLLS IT BACK
# cleanly if the server rejects — resyncing its GCD clock from CAST_FAILED.gcd_remaining_ms.
# It NEVER predicts the cast's OUTCOME (hit/crit/damage/heal) — that arrives as CAST_RESULT.
#
# KNOWN-ABILITY SET (the former greybox gap, now CLOSED by #457/#472): worldd pushes the
# character's REAL known ability set (KNOWN_ABILITIES 0x3005) to the owning client at
# ENTER_WORLD, and RE-pushes it after a TRAINER_LEARN that grows the set. The action bar
# seeds from publish_known_abilities() — each ability carries its server-authoritative
# `cast_ms` + `triggers_gcd` metadata, so the optimistic GCD/cast prediction no longer
# OVER-predicts (fixes #456): a `triggers_gcd:false` ability starts NO GCD, and a
# `cast_ms > 0` ability shows a real cast bar on CAST_START. A freshly-created character
# enters with an EMPTY set (M1: no durable ability table); it grows as it trains. The
# greybox set (hud/ability_set.gd) survives only as the headless-verify fixture.
#
# TIMING: the bus stores GCD/cast as a start stamp + duration in the caller's monotonic
# clock domain (Godot Time.get_ticks_msec(), passed in as `now_ms`). The bus itself does
# NO time math beyond max(0, start+duration-now) on demand — so it stays 100% event-driven
# and headlessly unit-testable (combat_verify.gd passes explicit timestamps). The VIEW owns
# the per-frame sweep animation (and enables _process only while a GCD/cast is live).

## The greybox predicted GCD length (ms). The server does NOT send the GCD duration on
## CAST_START (only cast_ms), so the optimistic prediction assumes this; CAST_FAILED's
## gcd_remaining_ms is the authoritative correction the bus snaps the clock to.
const GCD_MS := 1500

# world.fbs CastFailReason ordinals (kept here so views read them by name).
const CAST_FAIL_UNKNOWN_ABILITY := 0
const CAST_FAIL_NOT_IN_WORLD := 1
const CAST_FAIL_CASTER_DEAD := 2
const CAST_FAIL_ON_GCD := 3
const CAST_FAIL_ALREADY_CASTING := 4
const CAST_FAIL_INSUFFICIENT_RESOURCE := 5
const CAST_FAIL_NO_TARGET := 6
const CAST_FAIL_TARGET_DEAD := 7
const CAST_FAIL_WRONG_FACTION := 8
const CAST_FAIL_OUT_OF_RANGE := 9
const CAST_FAIL_NO_LINE_OF_SIGHT := 10
const CAST_FAIL_INTERRUPTED := 11

# world.fbs AttackOutcome ordinals (CAST_RESULT).
const OUTCOME_MISS := 0
const OUTCOME_DODGE := 1
const OUTCOME_PARRY := 2
const OUTCOME_HIT := 3
const OUTCOME_CRIT := 4

# --- View subscription signals (bus -> action bar / cast bar) ----------------

## The known-ability set changed (a KNOWN_ABILITIES push, or the greybox verify fixture).
## The action bar (re)builds its slots. `abilities` is an Array of ability Dictionaries
## {ability_id:int, name:String, icon_id:int, hotkey:int, cast_ms:int, triggers_gcd:bool,
## resource_type:int, resource_cost:int, range_m:float} — the server metadata drives the
## GCD/cast prediction (a triggers_gcd:false ability starts no GCD; cast_ms>0 casts).
signal ability_set_changed(abilities: Array)

## The GCD sweep changed. `start_ms`/`duration_ms` are in the Time.get_ticks_msec() domain
## the caller passed in; `duration_ms` == 0 means the GCD was cleared (a clean rollback).
## The action bar animates every slot's sweep from this window.
signal gcd_changed(start_ms: int, duration_ms: int)

## The cast bar changed. `active` false clears it (cast completed via CAST_RESULT, or was
## rejected/interrupted via CAST_FAILED). `duration_ms` is the server's cast_ms.
signal cast_bar_changed(active: bool, ability_id: int, start_ms: int, duration_ms: int)

## A CAST_FAILED reason arrived (world.fbs CastFailReason). The action/cast bar flashes a
## brief typed error; the GCD rollback is already applied via gcd_changed.
signal cast_failed_reason(ability_id: int, reason: int)

## A CAST_RESULT (server-authoritative attack-table resolution). Surfaced for a floating-
## text / nameplate hook (combat-presentation epic #23); the action bar only clears the
## cast bar on it. `result` carries {ability_id, caster_guid, target_guid, outcome, amount,
## is_heal, target_health, target_dead, server_time_ms}.
signal cast_result_received(result: Dictionary)

# --- Intent signal (view -> the world scene, which owns the net thread) -------

## A slot was pressed: send CAST_REQUEST for `ability_id` against `target_guid` (0 = self).
## `client_time_ms` is the press stamp (ClockSync-keyed on the wire).
signal cast_requested(ability_id: int, target_guid: int, client_time_ms: int)

# --- Combat registry state ---------------------------------------------------
var _abilities: Array = []       # known-ability set (KNOWN_ABILITIES; see the set note)
var _ability_meta: Dictionary = {}  # ability_id -> {cast_ms, triggers_gcd} for O(1) predict lookups
var _gcd_start_ms: int = 0       # optimistic GCD window start (caller's clock domain)
var _gcd_duration_ms: int = 0    # GCD window length (0 = no GCD running)
var _cast_active: bool = false   # a cast-time ability is in progress (cast bar visible)
var _cast_ability_id: int = 0
var _cast_start_ms: int = 0
var _cast_duration_ms: int = 0
var _pending_ability: int = 0    # ability whose CAST_REQUEST is in flight (0 = none)

# --- Publish / seed API (KNOWN_ABILITIES wire -> bus) -------------------------

## Publish a KNOWN_ABILITIES set (decode_cast_frame kind "known_abilities"): the character's
## REAL learned abilities + server metadata, pushed at ENTER_WORLD and re-pushed after a
## growing TRAINER_LEARN. Normalizes each wire row {ability_id, cast_ms, triggers_gcd,
## resource_type, resource_cost, range_m} into the canonical action-bar entry (deriving the
## display fields the wire omits: hotkey = slot order, name/icon from the id), replaces the
## whole set, and emits. An EMPTY set (a fresh character knows nothing) clears the bar.
func publish_known_abilities(abilities: Array) -> void:
	var out: Array = []
	for i in range(abilities.size()):
		var a: Dictionary = abilities[i]
		var aid := int(a.get("ability_id", 0))
		out.append({
			"ability_id": aid,
			"name": String(a.get("name", "Ability #%d" % aid)),
			"icon_id": int(a.get("icon_id", aid)),
			"hotkey": i + 1,  # the wire carries no keybind — slot order IS the hotkey
			"cast_ms": int(a.get("cast_ms", 0)),
			"triggers_gcd": bool(a.get("triggers_gcd", true)),
			"resource_type": int(a.get("resource_type", 0)),
			"resource_cost": int(a.get("resource_cost", 0)),
			"range_m": float(a.get("range_m", 0.0)),
		})
	_set_abilities(out)

## Seed the known-ability set directly (used by the headless verify fixture, which supplies
## already-canonical greybox entries). Replaces the whole set + emits. Prefer
## publish_known_abilities() for the real wire path (it normalizes wire rows).
func seed_abilities(abilities: Array) -> void:
	_set_abilities(abilities.duplicate(true))

# Store the ability set + rebuild the metadata index the GCD/cast prediction reads, then emit.
func _set_abilities(abilities: Array) -> void:
	_abilities = abilities
	_ability_meta.clear()
	for a in _abilities:
		var aid := int((a as Dictionary).get("ability_id", 0))
		_ability_meta[aid] = {
			"cast_ms": int((a as Dictionary).get("cast_ms", 0)),
			"triggers_gcd": bool((a as Dictionary).get("triggers_gcd", true)),
		}
	ability_set_changed.emit(self.abilities())

# --- Request API (view -> intent signal; the world scene sends the frame) -----

## A slot press. Emits the CAST_REQUEST intent and, for a GCD-triggering ability, predicts
## the GCD optimistically. An on-GCD press while a predicted GCD is still running is DROPPED
## (exactly as the server would reject ON_GCD — no wire spam). A `triggers_gcd:false` ability
## (KNOWN_ABILITIES metadata, #457) starts NO GCD and is NEVER dropped by the GCD guard —
## fixing the #456 over-prediction where every ability assumed a 1.5 s GCD. `now_ms` is
## Time.get_ticks_msec(). Returns true iff the cast was issued (the caller may flash the
## slot). Target is the current target (0 = self); the server resolves target legality.
func request_cast(ability_id: int, now_ms: int) -> bool:
	var triggers_gcd := _ability_triggers_gcd(ability_id)
	if triggers_gcd and gcd_remaining_ms(now_ms) > 0:
		return false  # predicted GCD busy — drop the on-GCD press (no wire spam)
	if triggers_gcd:
		# Optimistic GCD: start the sweep on the press so it feels instant (D-10).
		_gcd_start_ms = now_ms
		_gcd_duration_ms = GCD_MS
		gcd_changed.emit(_gcd_start_ms, _gcd_duration_ms)
	_pending_ability = ability_id
	cast_requested.emit(ability_id, _target_guid, now_ms)
	return true


# Whether `ability_id` triggers the global cooldown, per its KNOWN_ABILITIES metadata.
# Defaults to TRUE for an unknown id (the greybox fixture / an id not in the set) so the
# conservative "assume a GCD" behavior holds until real metadata says otherwise.
func _ability_triggers_gcd(ability_id: int) -> bool:
	if _ability_meta.has(ability_id):
		return bool((_ability_meta[ability_id] as Dictionary).get("triggers_gcd", true))
	return true

# --- Publish API (the net layer -> bus) --------------------------------------

## Publish a CAST_START (ACCEPT). Confirms the optimistic GCD and, for a cast-time ability
## (cast_ms > 0), starts the cast bar. An instant ability (cast_ms == 0) has no cast bar —
## its CAST_RESULT follows immediately. `now_ms` is Time.get_ticks_msec().
func publish_cast_start(ability_id: int, cast_ms: int, _server_time_ms: int, now_ms: int) -> void:
	_pending_ability = 0
	if cast_ms > 0:
		_cast_active = true
		_cast_ability_id = ability_id
		_cast_start_ms = now_ms
		_cast_duration_ms = cast_ms
		cast_bar_changed.emit(true, ability_id, now_ms, cast_ms)

## Publish a CAST_FAILED (REJECT). Rolls the optimistic GCD back / resyncs it from the
## authoritative `gcd_remaining_ms` (0 = no GCD → clean rollback; > 0 → snap the clock to
## the server's remaining GCD), clears any in-progress cast (INTERRUPTED etc.), and surfaces
## the typed reason. `now_ms` is Time.get_ticks_msec().
func publish_cast_failed(ability_id: int, reason: int, gcd_remaining_ms: int, now_ms: int) -> void:
	_pending_ability = 0
	if gcd_remaining_ms > 0:
		_gcd_start_ms = now_ms
		_gcd_duration_ms = gcd_remaining_ms
	else:
		_gcd_start_ms = 0
		_gcd_duration_ms = 0
	gcd_changed.emit(_gcd_start_ms, _gcd_duration_ms)
	if _cast_active:
		_clear_cast()
	cast_failed_reason.emit(ability_id, reason)

## Publish a CAST_RESULT (server-authoritative resolution). Clears the cast bar if this
## resolves the active cast, then re-emits the result for the combat-presentation hook
## (#23). The GCD is untouched (it continues to sweep to its predicted end). `now_ms`
## is accepted for signature symmetry (the result carries its own server_time_ms).
func publish_cast_result(result: Dictionary, _now_ms: int = 0) -> void:
	if _cast_active and _cast_ability_id == int(result.get("ability_id", -1)):
		_clear_cast()
	cast_result_received.emit(result.duplicate(true))

# --- Read API (bus -> view-models; all return copies / scalars) --------------

## The known-ability set (copies), in slot order.
func abilities() -> Array:
	return _abilities.duplicate(true)

## The GCD window start stamp (caller's clock domain; 0 = no GCD).
func gcd_start_ms() -> int:
	return _gcd_start_ms

## The GCD window length in ms (0 = no GCD running).
func gcd_duration_ms() -> int:
	return _gcd_duration_ms

## Remaining GCD in ms at `now_ms` (0 = ready). Computed on demand so the bus needs no
## clock of its own — the view calls this each animation frame with Time.get_ticks_msec().
func gcd_remaining_ms(now_ms: int) -> int:
	if _gcd_duration_ms <= 0:
		return 0
	return maxi(0, _gcd_start_ms + _gcd_duration_ms - now_ms)

## True while a cast-time ability is in progress (the cast bar is visible).
func cast_active() -> bool:
	return _cast_active

## The casting ability id (0 = none).
func cast_ability_id() -> int:
	return _cast_ability_id

## The cast window start stamp (caller's clock domain).
func cast_start_ms() -> int:
	return _cast_start_ms

## The cast window length in ms (the server's cast_ms).
func cast_duration_ms() -> int:
	return _cast_duration_ms

# --- Combat internals --------------------------------------------------------

# Clear the active cast + emit the cast-bar-hidden signal. Idempotent-ish (callers guard).
func _clear_cast() -> void:
	_cast_active = false
	var ability := _cast_ability_id
	_cast_ability_id = 0
	_cast_start_ms = 0
	_cast_duration_ms = 0
	cast_bar_changed.emit(false, ability, 0, 0)
