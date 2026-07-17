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
# Action bar + cast bar (CMB-01, D-10, #432) — same MVVM binding as the unit frames.
const ActionBar := preload("res://hud/action_bar.gd")
const CastBar := preload("res://hud/cast_bar.gd")
# XP bar + level-up presentation (CHR-03, #531) — same MVVM binding as the unit frames.
const XpBar := preload("res://hud/xp_bar.gd")
# Quest/gossip views (QST-01, #433) — same MVVM binding as the unit frames.
const GossipWindow := preload("res://hud/gossip_window.gd")
const QuestLogWindow := preload("res://hud/quest_log_window.gd")
const QuestTracker := preload("res://hud/quest_tracker.gd")
# Loot/vendor/trainer/bags views (ITM/ECO/NPC, #441) — same MVVM binding.
const LootWindow := preload("res://hud/loot_window.gd")
const VendorWindow := preload("res://hud/vendor_window.gd")
const TrainerWindow := preload("res://hud/trainer_window.gd")
const BagsWindow := preload("res://hud/bags_window.gd")
# Character sheet (#870, epic #866) — paperdoll + equipment slots; same MVVM binding.
const CharacterSheetWindow := preload("res://hud/character_sheet_window.gd")
# Chat panel (SOC-01, #434) — same MVVM binding as the other views.
const ChatPanel := preload("res://hud/chat_panel.gd")
const DeathOverlay := preload("res://hud/death_overlay.gd")

var _bus: MeridianEventBus
var _player_frame: MeridianUnitFrame
var _target_frame: MeridianUnitFrame
var _action_bar: MeridianActionBar
var _cast_bar: MeridianCastBar
var _xp_bar: MeridianXpBar
var _gossip: MeridianGossipWindow
var _quest_log: MeridianQuestLogWindow
var _quest_tracker: MeridianQuestTracker
var _loot: MeridianLootWindow
var _vendor: MeridianVendorWindow
var _trainer: MeridianTrainerWindow
var _bags: MeridianBagsWindow
var _character_sheet: MeridianCharacterSheetWindow
var _chat: MeridianChatPanel
var _death_overlay: MeridianDeathOverlay


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
	# Loot/vendor/trainer/bags windows subscribe to the SAME bus (ITM/ECO/NPC, #441).
	if _loot != null:
		_loot.setup(bus)
	if _vendor != null:
		_vendor.setup(bus)
	if _trainer != null:
		_trainer.setup(bus)
	if _bags != null:
		_bags.setup(bus)
	if _character_sheet != null:
		_character_sheet.setup(bus)
	# Chat panel subscribes to the SAME bus (SOC-01, #434).
	if _chat != null:
		_chat.setup(bus)
	# Death overlay subscribes to the SAME bus (CMB-03, #359/#532).
	if _death_overlay != null:
		_death_overlay.setup(bus)
	# Action bar + cast bar subscribe to the SAME bus (CMB-01, D-10, #432).
	if _action_bar != null:
		_action_bar.setup(bus)
	if _cast_bar != null:
		_cast_bar.setup(bus)
	# XP bar + level-up presentation subscribe to the SAME bus (CHR-03, #531).
	if _xp_bar != null:
		_xp_bar.setup(bus)
	# Paint the initial state (the bus may already hold vitals from queued frames).
	_refresh_player()
	_refresh_target()


# Fire action-bar slot `index` (0-based) — the world scene routes number keys 1..N here
# (CMB-01, #432). No-op before the bar is built / bound.
func press_action_slot(index: int) -> void:
	if _action_bar != null:
		_action_bar.press_slot(index)


# Toggle the quest log window (bound to a HUD key by the world scene, QST-01 #433).
func toggle_quest_log() -> void:
	if _quest_log != null:
		_quest_log.toggle()


# Toggle the bags/inventory window (bound to a HUD key by the world scene, ITM-01 #441).
func toggle_bags() -> void:
	if _bags != null:
		_bags.toggle()


# Toggle the character sheet (bound to [C] by the world scene, #870/#866).
func toggle_character_sheet() -> void:
	if _character_sheet != null:
		_character_sheet.toggle()


# Focus the chat input so the player can type (bound to Enter by the world scene, SOC-01 #434).
func focus_chat_input() -> void:
	if _chat != null:
		_chat.focus_input()


# True while the chat text input has keyboard focus — the world scene checks this to suppress
# gameplay keybinds (WASD / number keys / TAB) while the player is typing (SOC-01 #434).
func is_chat_input_focused() -> bool:
	return _chat != null and _chat.is_input_focused()


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

	# Loot window: centered-right, opens on a corpse (default hidden, ITM-02 #441).
	_loot = LootWindow.new()
	_loot.name = "LootWindow"
	_loot.set_anchors_preset(Control.PRESET_CENTER)
	_loot.position = Vector2(-150.0, -60.0)
	add_child(_loot)

	# Vendor window: center, opens from the gossip vendor entry (default hidden, ECO-01 #441).
	_vendor = VendorWindow.new()
	_vendor.name = "VendorWindow"
	_vendor.set_anchors_preset(Control.PRESET_CENTER)
	_vendor.position = Vector2(-180.0, -160.0)
	add_child(_vendor)

	# Trainer window: center, opens from the gossip trainer entry (default hidden, NPC-02 #441).
	_trainer = TrainerWindow.new()
	_trainer.name = "TrainerWindow"
	_trainer.set_anchors_preset(Control.PRESET_CENTER)
	_trainer.position = Vector2(-170.0, -140.0)
	add_child(_trainer)

	# Bags window: bottom-right, toggled with [B] (default hidden, ITM-01 #441).
	_bags = BagsWindow.new()
	_bags.name = "BagsWindow"
	_bags.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	_bags.position = Vector2(-320.0, -220.0)
	_bags.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	_bags.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(_bags)

	# Character sheet: left-center, toggled with [C] (default hidden, #870). Kept clear of
	# the bags window (bottom-right) so both can sit open while equipping from bags.
	_character_sheet = CharacterSheetWindow.new()
	_character_sheet.name = "CharacterSheetWindow"
	_character_sheet.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	_character_sheet.position = Vector2(24.0, -220.0)
	add_child(_character_sheet)

	# Action bar: bottom-center, always visible (CMB-01, #432). Anchored to the bottom edge
	# and grown upward so the row of slots sits above the screen bottom.
	_action_bar = ActionBar.new()
	_action_bar.name = "ActionBar"
	_action_bar.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	_action_bar.position = Vector2(-146.0, -74.0)
	_action_bar.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(_action_bar)

	# Cast bar: centered above the action bar, hidden until a cast-time ability starts.
	_cast_bar = CastBar.new()
	_cast_bar.name = "CastBar"
	_cast_bar.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	_cast_bar.position = Vector2(-130.0, -104.0)
	_cast_bar.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(_cast_bar)

	# XP bar: bottom-center, always visible (CHR-03, #531). Anchored to the bottom edge and
	# grown upward so the slim progress strip sits just below the action bar. The level-up
	# burst draws ABOVE it (the burst label is offset up within the view).
	_xp_bar = XpBar.new()
	_xp_bar.name = "XpBar"
	_xp_bar.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	_xp_bar.position = Vector2(-XpBar.BAR_W * 0.5, -18.0)
	_xp_bar.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(_xp_bar)

	# Chat panel: bottom-left, always visible (SOC-01, #434). Anchored to the bottom-left
	# corner and grown upward so the scrollback + input row sit above the screen bottom.
	_chat = ChatPanel.new()
	_chat.name = "ChatPanel"
	_chat.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_chat.position = Vector2(12.0, -224.0)
	_chat.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(_chat)

	# Death overlay: full-screen, added LAST so its dim/greyscale wash sits over the whole HUD
	# + world (CMB-03, #359/#532). Hidden until a DEATH_STATE arrives.
	_death_overlay = DeathOverlay.new()
	_death_overlay.name = "DeathOverlay"
	add_child(_death_overlay)

	# If setup() ran before _build() (node added after bind), paint + bind now.
	if _bus != null:
		_gossip.setup(_bus)
		_quest_log.setup(_bus)
		_quest_tracker.setup(_bus)
		_loot.setup(_bus)
		_vendor.setup(_bus)
		_trainer.setup(_bus)
		_bags.setup(_bus)
		_character_sheet.setup(_bus)
		_chat.setup(_bus)
		_action_bar.setup(_bus)
		_cast_bar.setup(_bus)
		_xp_bar.setup(_bus)
		_death_overlay.setup(_bus)
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
