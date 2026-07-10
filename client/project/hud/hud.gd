# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the HUD root (UI-01, #431). A CanvasLayer that sits OVER the 3D
# world viewport and hosts the unit frames: the LOCAL PLAYER frame and the TARGET
# frame. It is the ViewModel binding layer — it subscribes to the MeridianEventBus
# and drives the two pure MeridianUnitFrame views.
#
# THE ONLY PLACE THE HUD READS SERVER STATE IS THE EVENT BUS. The HUD holds no net
# handle and decodes nothing; world.gd publishes server events into the bus, and the
# bus's signals drive this class. Add more frames/panels here (action bars, cast bar,
# nameplates — epic #24) by subscribing to the same bus; never by reaching into the
# net thread.
#
# EVENT-DRIVEN (≤2 ms budget, #431): no _process. Each handler runs only when the bus
# emits, and touches one frame. Binding a frame = one render() call.
class_name MeridianHud
extends CanvasLayer

const UnitFrame := preload("res://hud/unit_frame.gd")
# Quest/gossip views (QST-01, #433) — same MVVM binding as the unit frames.
const GossipWindow := preload("res://hud/gossip_window.gd")
const QuestLogWindow := preload("res://hud/quest_log_window.gd")
const QuestTracker := preload("res://hud/quest_tracker.gd")

var _bus: MeridianEventBus
var _player_frame: MeridianUnitFrame
var _target_frame: MeridianUnitFrame
var _gossip: MeridianGossipWindow
var _quest_log: MeridianQuestLogWindow
var _quest_tracker: MeridianQuestTracker


func _ready() -> void:
	_build()


# Bind the HUD to the world session's event bus. Call once, before or after the node
# enters the tree. Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.local_player_changed.connect(_on_local_player_changed)
	_bus.target_changed.connect(_on_target_changed)
	_bus.entity_vitals_changed.connect(_on_entity_vitals_changed)
	# Quest/gossip windows subscribe to the SAME bus (QST-01, #433).
	if _gossip != null:
		_gossip.setup(bus)
	if _quest_log != null:
		_quest_log.setup(bus)
	if _quest_tracker != null:
		_quest_tracker.setup(bus)
	# Paint the initial state (the bus may already hold vitals from queued frames).
	_refresh_player()
	_refresh_target()


# Toggle the quest log window (bound to a HUD key by the world scene, QST-01 #433).
func toggle_quest_log() -> void:
	if _quest_log != null:
		_quest_log.toggle()


func _build() -> void:
	if _player_frame != null:
		return
	# Player frame: top-left, under the #301 diagnostic text panel.
	_player_frame = UnitFrame.new()
	_player_frame.name = "PlayerFrame"
	_player_frame.position = Vector2(12, 108)
	add_child(_player_frame)

	# Target frame: to the right of the player frame. Hidden until a target is set.
	_target_frame = UnitFrame.new()
	_target_frame.name = "TargetFrame"
	_target_frame.position = Vector2(270, 108)
	_target_frame.set_frame_visible(false)
	add_child(_target_frame)

	# Quest tracker: top-right, always-on for the watched quest (QST-01, #433).
	_quest_tracker = QuestTracker.new()
	_quest_tracker.name = "QuestTracker"
	_quest_tracker.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_quest_tracker.position = Vector2(-250.0, 12.0)
	_quest_tracker.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	add_child(_quest_tracker)

	# Quest log window: right side, toggled (default hidden).
	_quest_log = QuestLogWindow.new()
	_quest_log.name = "QuestLogWindow"
	_quest_log.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_quest_log.position = Vector2(-360.0, 160.0)
	_quest_log.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	add_child(_quest_log)

	# Gossip window: centered-left, opens on a targeted NPC (default hidden).
	_gossip = GossipWindow.new()
	_gossip.name = "GossipWindow"
	_gossip.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	_gossip.position = Vector2(40.0, -80.0)
	add_child(_gossip)

	# If setup() ran before _build() (node added after bind), paint + bind now.
	if _bus != null:
		_gossip.setup(_bus)
		_quest_log.setup(_bus)
		_quest_tracker.setup(_bus)
		_refresh_player()
		_refresh_target()


# --- Bus signal handlers -----------------------------------------------------

func _on_local_player_changed(_guid: int) -> void:
	_refresh_player()


func _on_target_changed(_guid: int) -> void:
	_refresh_target()


func _on_entity_vitals_changed(guid: int, vitals: Dictionary) -> void:
	# Only the two bound units affect the HUD — everything else is a no-op.
	if _bus == null:
		return
	if guid == _bus.local_guid() and _player_frame != null:
		_player_frame.render(vitals)
	if guid == _bus.target_guid() and _target_frame != null:
		_target_frame.render(vitals)


# --- Binding helpers ---------------------------------------------------------

func _refresh_player() -> void:
	if _bus == null or _player_frame == null:
		return
	_player_frame.render(_bus.local_vitals())


func _refresh_target() -> void:
	if _bus == null or _target_frame == null:
		return
	var has_target := _bus.target_guid() != 0
	_target_frame.set_frame_visible(has_target)
	if has_target:
		_target_frame.render(_bus.target_vitals())
