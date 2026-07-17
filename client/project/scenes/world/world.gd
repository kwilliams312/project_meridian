# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the NETWORKED world scene (issue #301). The capstone that turns
# the GUI into a real networked client: after login + character select, this scene
# connects to the selected realm's worldd over the dedicated net thread and RENDERS
# remote entities (a bot, or a second client) MOVING on screen.
#
# It wires the proven client pieces into one live loop (Client SAD §2.2/§6.1):
#   * MeridianNetThread (#97/#279) — owns the IF-2 world session on its own thread
#     (the worldd socket lives there); connect_to_world() sends WorldHello, reads
#     HandshakeOk, and each physics tick pump() drains decoded server events as
#     signals at the fixed pre-sim sync point.
#   * MeridianMovementController (#102/#103) — the LOCAL player: input → predicted
#     MovementIntent to worldd; server MovementState reconciles (predict/rewind).
#   * MeridianRemoteInterpolator (#104) — every OTHER player: EntityEnter spawns a
#     remote node, EntityUpdate feeds smoothed interpolation, EntityLeave despawns.
#   * MeridianTpsCamera (#105) — the third-person WoW camera on the local player.
#
# The headless proof of the SAME net path (this scene's connect → relay drain) is
# client/test/run_client_sees_bot_it.sh (meridian-client-probe); the on-screen watch
# is scripts/dev/demo-networked.sh. camera_demo.tscn stays the standalone local demo.
#
# COORDINATE FRAMES: worldd is Z-UP (wire x/y = ground, z = height); Godot is Y-UP
# (y = height). MeridianNetThread.decode_entity_frame() already maps relay positions
# into the Godot frame, and build_movement_intent_frame() maps intents back to wire,
# so this script works purely in Godot space.
#
# Enter with configure(session, character) BEFORE the scene is added to the tree; a
# missing/empty session runs an OFFLINE local sandbox (no worldd) so the scene is
# still openable standalone.

extends Node3D

# The M0 bootstrap spawn: worldd seeds every session at the Zone-01 play-area centre,
# wire (-320, -320, 0) — Godot (x=-320, y=height 0, z=-320). Kept BYTE-ALIGNED with the
# server (movement::kZoneSpawnXY = -320, #562) and the headless bot (bot_core.cpp
# kSpawnWireX/Y = -320) so the very first WASD move lands INSIDE the movement bounds
# [-512, -128] instead of being rejected out-of-bounds (#805). worldd is Z-UP (wire x/y =
# ground plane, z = height); Godot is Y-UP, so wire (x, y) -> Godot (x, z) and y = height.
# The local player + camera start here.
const SPAWN := Vector3(-320.0, 0.0, -320.0)

# Fixed sim cadence (matches the server 20 Hz / 50 ms tick the controller expects).
const TICK_MS := 50

# Click-to-target (CMB-01, #496). Every remote entity carries a pickable collider on a
# DEDICATED physics layer so a mouse raycast can select it WITHOUT disturbing the
# camera's collision boom (the SpringArm3D probes layer 1). The target-pick ray masks
# ONLY this layer, and the colliders detect nothing themselves (mask 0) — they are pure
# ray targets, never a physics obstacle for movement or the camera.
const TARGET_PHYS_LAYER := 2          # physics layer bit the targetable colliders live on
const TARGET_PICK_RANGE := 1000.0     # metres — max click-pick ray length
# A left-press that moves fewer than this many pixels before release is a CLICK (select),
# not a camera-orbit DRAG — so click-to-target and left-drag orbit coexist on the SAME
# button (the TPS camera orbits on left-drag). We never mark the LMB event handled.
const TARGET_CLICK_DRAG_PX := 8.0

# NPC dialog auto-close (#851). While a gossip / vendor / trainer window is open, the world
# scene closes it once the player walks out of the NPC's interaction reach — you should not
# keep a menu open from across the map. The reach + the planar distance decision live in the
# dependency-free DialogAutoclose module (mirrors the server gate #842; 5 m open, 5.5 m close
# with hysteresis) so the policy is unit-testable without the whole world scene. The server
# stays authoritative (it already refuses out-of-range gossip/accept/turn-in); this is purely
# the client proactively closing the view.
const DialogAutoclose := preload("res://scenes/world/dialog_autoclose.gd")

# Character Select — where a PRE-HandshakeOk connection failure returns the player
# (#301 UX: never strand them in an empty world). Kept in sync with the char-select
# handoff (scenes/charselect/char_select.gd → WORLD_SCENE).
const CHAR_SELECT_SCENE := "res://scenes/charselect/char_select.tscn"

# The single shared class→color table (#328). BOTH the local player capsule and the
# remote-player capsules resolve their color through this one script, so own vs.
# remote coloring can never drift — every client renders a class the same way.
const PlayerClassColors := preload("res://scenes/world/player_class_colors.gd")
# WASD → world-space move basis (CHR-02, #619): keeps "forward" locked to the
# character's facing for ALL yaws (Godot Y-rotation, shared with input_move_verify).
const MovementBasis := preload("res://scenes/world/movement_basis.gd")

# HUD foundation (UI-01, #431): the MVVM event bus + the unit-frame HUD. world.gd
# owns the ONE event bus for this world session and publishes every decoded server
# unit event into it; the HUD subscribes to the bus and never touches the net thread.
const MeridianEventBusScript := preload("res://hud/event_bus.gd")
const MeridianHudScript := preload("res://hud/hud.gd")
# Floating chat bubble (SOC-01, #434) — attached to an entity node for say/yell lines.
const ChatBubbleScript := preload("res://hud/chat_bubble.gd")
# Floating combat text (CMB-04, #530) — a POOLED set of billboarded numbers the world
# scene floats over a target when a CAST_RESULT resolves (driven off the event bus's
# cast_result_received seam). Pure presentation; server-authoritative.
const FloatingCombatTextScript := preload("res://scenes/world/floating_combat_text.gd")
# Nameplates (CMB-03, #535) — a POOLED set of billboarded name+health-bar plates the world
# scene attaches over each visible remote entity. Name from ENTITY_ENTER, health tracked off
# the SAME event-bus vitals seam (entity_vitals_changed) the unit frames read. Server-authoritative.
const NameplateManagerScript := preload("res://scenes/world/nameplate_manager.gd")
# Assembled characters (②/T4, #541): when an EntityEnter carries appearance (a player
# entity), the local player and each remote render an AssembledCharacter — the per-race
# body + worn gear built from the wire ids via MeridianContentDB — instead of the
# class-colored capsule. The capsule stays the FALLBACK: no appearance on the frame (NPC
# / old server), assemble() returns false (no catalog for the race), or missing content
# (spec §6). Preloaded by path (never the bare class name) so headless --script verifies,
# which don't populate the global class cache, resolve it identically to the running app.
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")

# Enter-world terrain streaming (WLD-01, Epic #22 Story E, #558). The full chain —
# fail-closed pack mount+verify (A/#554), the MeridianChunkStream chunk root (B/#555)
# fed the predicted player position, its proxy far-ring + hitch gate (C/#556), and the
# HeightfieldWorldQuery movement ground-sample (D/#557) — replaces the M0 flat bootstrap
# box so the client spawns on STREAMED terrain. The loading screen (below) holds until
# the spawn chunks are resident, then reveals; seamless thereafter (client-prd §M1:
# a loading screen only on a map change). When NO zone pack is configured/present (the
# default today, until the content pipeline ships real Zone-01 — Forge #26/#315), the
# scene falls back to the flat bootstrap so the networked demo keeps working.
const WorldLoadingScreenScript := preload("res://scenes/world/world_loading_screen.gd")

# Where a theme's zone packs live: res://meridian/<theme>/chunks/<zone>/ (the by-ID
# pack layout mcc's chunk-emit writes and check-golden stages). The chunk manifest is
# `<dir>/<id>.chunks.json` + the asset table `<dir>/<id>.assets.json` (IF-6), and the
# `.chunk.bin` server heightfield per cell (Q1(a)) feeds the mover.
#
# The zone is DISCOVERED under the realm's theme rather than hardcoded (#877): the
# theme already selects which pack the client mounts (MeridianContentDB.resolve_theme
# — MERIDIAN_REALM_THEME, mirroring worldd's primary pack_namespace), so a theme that
# ships terrain streams it and one that doesn't falls back to the flat bootstrap. That
# keeps zone content out of client code — "pack = the theme" (#648) — and is why
# MERIDIAN_REALM_THEME=chibi is all it takes to land in Sprout Meadow.
#
# NOTE this is the CLIENT path only. The zone's own `chunk_manifest:` field is still
# schema-pinned to `type: "null"` (schema/content/zone.schema.yaml — "RESERVED, A-08"),
# and un-reserving it (schema + 80_zone.sql + content_types.gen.hpp + the server's
# manifest-origin translation) is the M0-exit epic #874, deliberately NOT pulled in here.
const ZONE_CHUNKS_SUBDIR := "chunks"

# The per-frame chunk instancing budget the streamer honours (Story C hitch gate:
# ≤ 50 ms streaming/frame). Conservative default until the perf fleet (#31) tunes it.
const ZONE_INSTANCE_BUDGET := 4

var _session: Dictionary = {}
var _character: Dictionary = {}

var _net: MeridianNetThread
var _net_preconnected: bool = false  # net thread handed over LIVE by char-select (D-35)
var _interp: MeridianRemoteInterpolator
var _mover: MeridianMovementController

# Pure routing decision for a connection outcome (unit-tested headlessly by
# scenes/world/world_connect_router_verify.gd). Feeds on the connection signals and
# tells us whether a failure should bounce back to Character Select.
var _router: MeridianWorldConnectRouter
var _routed_away := false                   # one-shot guard: route back exactly once

var _player: Node3D
var _body: Node3D
var _camera: Node3D

var _remotes: Node3D                       # container for remote-player nodes
var _remote_nodes: Dictionary = {}         # guid:int -> Node3D

# --- Enter-world terrain streaming (WLD-01, Story E, #558) --------------------
# The streamer node whose children ARE the resident chunks (replaces the flat
# ground box), the loading overlay, and the zone bookkeeping that ties the render
# residency to the movement heightfield residency so nothing falls through.
var _chunk_stream: MeridianChunkStream     # the streamed chunk root (null in the flat-bootstrap fallback)
var _loading_screen: MeridianWorldLoadingScreen
var _zone_active := false                  # a zone pack mounted + streaming (else flat bootstrap)
var _zone_loading := false                 # still holding the loading screen (spawn chunks not yet resident)
var _zone_id := ""                         # display id for the loading screen
var _zone_origin := Vector3.ZERO           # IF-6 zone origin (x, _, z) — grid geometry
var _zone_chunk_size := 128.0              # IF-6 chunk_size_m
var _zone_spawn := SPAWN                   # where the local player enters this zone (server-authoritative in production)
var _zone_server_paths: Dictionary = {}    # Vector2i(cx,cz) -> res://…/<cell>.chunk.bin (the heightfield payload)
var _hf_loaded_cells: Dictionary = {}      # Vector2i(cx,cz) -> true once its heightfield is resident on the mover

# Floating combat text (CMB-04, #530): the pooled billboarded-number system, spawned
# over a target guid on cast_result_received. World-space child (never reparented).
var _floating_text: Node3D

# Nameplates (CMB-03, #535): the pooled name+health-bar plate system. Plates are reparented
# ONTO each remote entity node on spawn and recycled back to the pool on despawn.
var _nameplates: Node3D

# Click-to-target (#496): a single reusable selection ring reparented under / positioned
# over the current target, plus the LMB click-vs-drag tracker (a drag orbits the camera;
# a clean click selects). The ring shows only for a REMOTE target that is in AoI.
var _target_ring: MeshInstance3D
var _lmb_down := false
var _lmb_drag_px := 0.0
var _lmb_press_screen := Vector2.ZERO
var _lmb_press_on_ui := false

var _hud_state: Label
var _hud_guid: Label
var _hud_remotes: Label
var _hud_tick: Label

# UI-01 HUD foundation (#431): the server-state registry + the unit-frame HUD view.
var _bus: MeridianEventBus
var _hud: MeridianHud

var _my_guid: int = 0
var _last_server_tick: int = 0
var _conn_text: String = "offline"
var _client_ms: int = 0                     # monotonic client clock for the sim

# Local-movement diagnostic (#303). OFF by default; the OWNER enables it with the
# MERIDIAN_MOVE_DEBUG env var (any non-empty value) before launching the client to
# get a once-per-second line proving whether WASD input is even being read (rules
# out window focus) and whether the mover's render position advances (rules out the
# mover) vs. a server correction pulling the capsule back to spawn. Throttled to ~1
# Hz off the sim clock so it never floods the log; the read is a couple of atomics.
var _move_debug: bool = false
var _last_move_debug_ms: int = -100000


func configure(session: Dictionary, character: Dictionary = {}) -> void:
	_session = session if session != null else {}
	_character = character if character != null else {}
	# Server-authoritative characters (D-35 / #279): char-select established the world
	# session (WorldHello → CharList → ENTER_WORLD OK) on a LIVE net thread and handed
	# it over here. Reuse it — the grant is single-use, so we must NOT reconnect.
	if _session.get("net_thread", null) != null:
		_net = _session.get("net_thread")
		_net_preconnected = true


func _ready() -> void:
	_move_debug = not OS.get_environment("MERIDIAN_MOVE_DEBUG").is_empty()
	if _move_debug:
		print("[world.move] MERIDIAN_MOVE_DEBUG on — logging local-player input/render once per second")
	_build_environment()
	# Enter-world terrain (Story E, #558): stream the zone's chunks when a pack is
	# configured/present, else fall back to the M0 flat bootstrap box. Built BEFORE
	# the player so the chunk root + loading screen exist when the mover is wired.
	_build_loading_screen()
	_build_world_terrain()
	_build_player_and_camera()
	_remotes = Node3D.new()
	_remotes.name = "Remotes"
	add_child(_remotes)
	_build_hud()

	# UI-01 (#431): stand up the event bus + unit-frame HUD BEFORE wiring the net
	# signals, so the very first drained EntityEnter/VITALS_UPDATE can publish into a
	# live bus. The HUD subscribes to the bus; it never sees the net thread.
	_bus = MeridianEventBusScript.new()
	_hud = MeridianHudScript.new()
	_hud.name = "UnitFrameHud"
	add_child(_hud)
	_hud.setup(_bus)

	# Click/tab-target (CMB-01, #496): the world scene owns the 3D nodes, so it drives the
	# on-screen SELECTION VISUAL (the ring) off the bus's target_changed. Selection itself
	# still flows through the bus (set_target), exactly like the target unit frame's binding.
	_bus.target_changed.connect(_on_target_changed)
	_build_target_ring()

	# QST-01 (#433): the world scene owns BOTH the bus and the net thread, so it is the
	# controller that turns a UI INTENT (a bus request_* signal) into an outbound frame,
	# and a decoded quest/gossip server frame into a bus publish. The HUD windows only
	# ever see the bus — the net thread stays behind this one seam (extends #431).
	_bus.gossip_hello_requested.connect(_on_gossip_hello_requested)
	_bus.quest_accept_requested.connect(_on_quest_accept_requested)
	_bus.quest_turn_in_requested.connect(_on_quest_turn_in_requested)
	_bus.quest_log_requested.connect(_on_quest_log_requested)
	_bus.giver_indicator_changed.connect(_on_giver_indicator_changed)

	# ITM/ECO/NPC (#441): the same controller seam for loot/vendor/trainer. The gossip
	# vendor/trainer entry hooks open the sibling windows; loot/vendor/trainer intents
	# become outbound frames; the server's typed results are decoded + published back.
	_bus.vendor_entry_selected.connect(_on_vendor_entry_selected)
	_bus.trainer_entry_selected.connect(_on_trainer_entry_selected)
	_bus.loot_request_requested.connect(_on_loot_request_requested)
	_bus.loot_take_requested.connect(_on_loot_take_requested)
	_bus.loot_release_requested.connect(_on_loot_release_requested)
	_bus.vendor_buy_requested.connect(_on_vendor_buy_requested)
	_bus.vendor_sell_requested.connect(_on_vendor_sell_requested)
	_bus.vendor_buyback_requested.connect(_on_vendor_buyback_requested)
	_bus.trainer_learn_requested.connect(_on_trainer_learn_requested)
	_bus.equipment_change_requested.connect(_on_equipment_change_requested)
	# Combat intent (CMB-01, D-10, #432): a slot press → CAST_REQUEST frame.
	_bus.cast_requested.connect(_on_cast_requested)
	# Combat presentation (CMB-04, #530): the server-authoritative CAST_RESULT the bus
	# re-emits (cast_result_received) floats a pooled damage/heal number over the target.
	# The world scene owns the 3D nodes + the guid→Node3D map, so it drives this visual.
	_build_floating_text()
	_bus.cast_result_received.connect(_on_cast_result)
	# Nameplates (CMB-03, #535): the pooled name+health-bar plate system. Plates are attached
	# in _spawn_remote (needs the entity node) and recycled in _despawn_remote; their health
	# tracks the SAME entity_vitals_changed seam the unit frames read (no duplicate signal).
	_build_nameplates()
	_bus.entity_vitals_changed.connect(_on_nameplate_vitals)
	# Chat (SOC-01, #434): a submitted line → CHAT_MESSAGE frame; a delivered SAY/YELL line
	# floats a bubble over the sender's entity. CHAT_DELIVER/CHAT_REJECTED are decoded +
	# published back through the SAME bus (never invented client-side — the sender even sees
	# its own say/yell because worldd echoes it).
	_bus.chat_send_requested.connect(_on_chat_send_requested)
	_bus.chat_bubble_requested.connect(_on_chat_bubble_requested)
	# Death loop (CMB-03, #359/#532): the death overlay's Release / Resurrect intents become
	# the empty RELEASE_REQUEST / RESURRECT_REQUEST frames. DEATH_STATE / GHOST_STATE /
	# RESURRECT_RESULT are decoded + published back through the SAME bus (never predicted).
	_bus.release_requested.connect(_on_release_requested)
	_bus.resurrect_requested.connect(_on_resurrect_requested)

	_interp = MeridianRemoteInterpolator.new()
	_mover = MeridianMovementController.new()
	_mover.reset(SPAWN, 0.0)
	# Story E (#558): now that the mover exists, swap its ground-sample backend to the
	# HeightfieldWorldQuery over the mounted zone (D/#557). No-op in the flat-bootstrap
	# fallback (FlatWorldQuery stays). Loads the spawn-area heightfields as they stream.
	_apply_zone_to_mover()

	if _net_preconnected and _net != null:
		_attach_preconnected()
	elif _has_session():
		_connect_to_world()
	else:
		_conn_text = "offline (no session — local sandbox)"
		print("[world] no session context — running OFFLINE local sandbox")
	_refresh_hud()


# Reuse the LIVE net thread handed over by char-select (D-35 / #279). HandshakeOk +
# ENTER_WORLD already happened there, so no handshake_ok signal will arrive here — we
# mark ourselves in-world and let _physics_process pump() drain the queued AoI
# EntityEnter frames. Wire the same signals _connect_to_world() would have.
func _attach_preconnected() -> void:
	_router = MeridianWorldConnectRouter.new()
	_net.handshake_ok.connect(_on_handshake_ok)  # harmless if a late one arrives
	_net.movement_state.connect(_on_movement_state)
	_net.entity_frame.connect(_on_entity_frame)
	_net.disconnected.connect(_on_disconnected)
	_net.transport_closed.connect(_on_transport_closed)
	_net.connect_failed.connect(_on_connect_failed)
	print("[world] reusing the live world session from character-select (in-world)")
	_on_handshake_ok()  # we are already past char-select — treat as entered


func _exit_tree() -> void:
	if _net != null:
		_net.disconnect_from_world()


# --- Net session --------------------------------------------------------------

func _has_session() -> bool:
	return not _session.is_empty() \
		and _session.has("world_hello_frame") \
		and (_session.get("world_hello_frame") as PackedByteArray).size() > 0 \
		and not String(_session.get("worldd_host", "")).is_empty()


func _connect_to_world() -> void:
	_router = MeridianWorldConnectRouter.new()
	_net = MeridianNetThread.new()
	_net.handshake_ok.connect(_on_handshake_ok)
	_net.movement_state.connect(_on_movement_state)
	_net.entity_frame.connect(_on_entity_frame)
	_net.disconnected.connect(_on_disconnected)
	_net.transport_closed.connect(_on_transport_closed)
	_net.connect_failed.connect(_on_connect_failed)

	var host := String(_session.get("worldd_host", ""))
	var port := int(_session.get("worldd_port", 0))
	var frame: PackedByteArray = _session.get("world_hello_frame", PackedByteArray())
	var key: PackedByteArray = _session.get("session_key", PackedByteArray())
	_conn_text = "connecting to %s:%d…" % [host, port]
	print("[world] connecting to worldd %s:%d (world_hello=%d B, key=%d B)"
		% [host, port, frame.size(), key.size()])
	if not _net.connect_to_world(host, port, frame, key):
		_conn_text = "connect failed (bad WorldHello frame)"
		_handle_connection_event(MeridianWorldConnectRouter.EVENT_BAD_HELLO, "bad WorldHello frame")


func _physics_process(delta: float) -> void:
	_client_ms += TICK_MS

	# 1. Pre-sim sync point: drain every decoded server event (emits our signals).
	if _net != null:
		_net.pump()

	# 2. Local player: sample input, predict, send the intent to worldd.
	if _net != null and _net.is_in_world():
		_tick_local_player()
	elif _move_debug:
		# The local-player tick is GATED until HandshakeOk — if the owner sees only
		# these lines (never a [world.move] input line), WASD "not working" is really
		# "not in world yet", not an input/mover bug.
		_move_debug_gate()

	# 3. Remote players: sample the interpolator and write each node's position.
	_update_remotes()

	# 4. Render smoothing for the local prediction reconciliation (#103).
	if _mover != null:
		_mover.advance_smoothing(int(delta * 1000.0))
		if _player != null:
			_player.position = _mover.get_render_position()

	# 4b. Terrain streaming (Story E, #558): feed the streamer the PREDICTED player
	# position, instance up to the hitch-gated budget, keep the movement heightfield
	# resident with the render residency, and reveal once the spawn chunks are in.
	if _zone_active:
		_tick_world_stream()

	# 5. Corpse-run (CMB-03, #532): while a ghost, feed the local player's position to the bus
	# so it measures the run to the corpse and fires RESURRECT_REQUEST once within range. The
	# server still validates (RESURRECT_TOO_FAR) — this only completes the run without a click.
	if _bus != null and _bus.is_ghost() and _player != null:
		_bus.update_ghost_position(_player.position)

	# 6. NPC dialog auto-close (#851): if an interaction window is open and the player has
	# walked out of the NPC's interaction reach, proactively close it (mirrors the server
	# range gate #842). Runs after the remote + player positions are updated above.
	_tick_interaction_autoclose()

	_refresh_hud()


func _tick_local_player() -> void:
	# WASD is relative to the VIEW / look direction (camera_yaw), not the
	# character's committed facing: "forward" is always the way the camera points,
	# and it follows BOTH right-click steer AND left-click orbit. Physical keys so
	# there's no input-map dependency.
	var key_w := Input.is_physical_key_pressed(KEY_W)
	var key_s := Input.is_physical_key_pressed(KEY_S)
	var key_d := Input.is_physical_key_pressed(KEY_D)
	var key_a := Input.is_physical_key_pressed(KEY_A)
	var fwd := 0.0
	var strafe := 0.0
	if key_w: fwd += 1.0
	if key_s: fwd -= 1.0
	if key_d: strafe += 1.0
	if key_a: strafe -= 1.0

	var yaw := 0.0
	if _camera != null:
		yaw = _camera.get_camera_yaw()  # the VIEW yaw — "forward = where you look"
	# Rotate the local (strafe, forward) input into world axes by the VIEW yaw
	# using Godot's own Y-rotation (Basis(UP, yaw) * v), so W drives along the
	# camera's look direction and A/S/D are relative to it, for ALL yaws. The yaw
	# is also reported as the facing so remotes see the character oriented toward
	# where it moves/looks. Godot forward is -Z; a yaw of 0 faces -Z.
	var move := MovementBasis.character_relative_move(fwd, strafe, yaw)

	var intent: Dictionary = _mover.predict(move, false, false, yaw, _client_ms)
	if _mover.should_emit_intent(_client_ms, int(intent.get("state_flags", 0))):
		var frame: PackedByteArray = _net.build_movement_intent_frame(intent)
		if frame.size() > 0:
			_net.send_movement_intent(frame)

	if _move_debug:
		_emit_move_debug(key_w, key_a, key_s, key_d, move, yaw)


# --- Local-movement diagnostic (#303) ----------------------------------------
# One throttled line per second while in world. It pins, for the OWNER's next run,
# the two things a headless test cannot: (1) whether the keystrokes actually reach
# the window — the W/A/S/D bools go true only if the OS delivered the key to a
# focused client; (2) whether the mover's RENDER position (what the capsule draws
# at) advances with input — and how it compares to the raw predicted SIM and the
# last reconcile error, so a server that never advances the authoritative position
# (render creeps out, then a correction pulls it back) is visible as a large,
# recurring last_err instead of steady outward motion.
func _emit_move_debug(key_w: bool, key_a: bool, key_s: bool, key_d: bool,
		move: Vector3, yaw: float) -> void:
	if _client_ms - _last_move_debug_ms < 1000:
		return
	_last_move_debug_ms = _client_ms
	var render: Vector3 = _mover.get_render_position()
	var pred: Vector3 = _mover.get_predicted_position()
	# world = the capsule's ACTUAL drawn transform (_player.position is set from the
	# render position each frame). With the landmark grid on screen, comparing this
	# world coordinate tick-to-tick tells the owner whether the capsule really moves
	# in world space when W is held — the ground truth behind the parallax they see.
	var world: Vector3 = _player.position if _player != null else render
	print("[world.move] W=%s A=%s S=%s D=%s yaw=%.3f move=(%.2f,%.2f,%.2f) world=(%.2f,%.2f,%.2f) render=(%.2f,%.2f,%.2f) pred=(%.2f,%.2f,%.2f) last_err=%.3f srv_tick=%d"
		% [key_w, key_a, key_s, key_d, yaw,
			move.x, move.y, move.z,
			world.x, world.y, world.z,
			render.x, render.y, render.z,
			pred.x, pred.y, pred.z,
			_mover.last_error_magnitude(), _last_server_tick])


# Throttled note while the local-player tick is still gated behind HandshakeOk.
func _move_debug_gate() -> void:
	if _client_ms - _last_move_debug_ms < 1000:
		return
	_last_move_debug_ms = _client_ms
	var in_world: bool = _net != null and _net.is_in_world()
	print("[world.move] local-player tick GATED (net=%s in_world=%s conn=%s) — WASD is inert until in world"
		% [str(_net != null), str(in_world), _conn_text])


func _update_remotes() -> void:
	if _interp == null:
		return
	# #885: the server's ground model is still flat (kFlatGroundZ = 0), so every
	# remote's wire height arrives as 0 while the client renders real terrain —
	# remotes sink into any ground that rises. Snap their render height to the
	# same heightfield the local player is already snapped to (#557), exactly as
	# _try_reveal_world() does for us. Hoisted out of the loop: one null/active
	# check per frame, not per entity.
	var snap: bool = _mover != null and _mover.is_heightfield_active()
	for guid in _remote_nodes.keys():
		var node: Node3D = _remote_nodes[guid]
		if node == null:
			continue
		var sample: Dictionary = _interp.sample_entity(int(guid), _client_ms)
		if int(sample.get("kind", 0)) != 0:  # 0 = empty (nothing buffered yet)
			var x := float(sample.get("x", 0.0))
			var y := float(sample.get("y", 0.0))
			var z := float(sample.get("z", 0.0))
			# Only the HEIGHT comes from terrain — x/z keep the interpolator's
			# smoothing untouched (no jitter on slopes). Ground that is not
			# walkable (cell not streamed in yet, or a hole) has no meaningful
			# height: keep the wire y rather than slamming the entity to 0.
			if snap:
				var g: Dictionary = _mover.sample_ground(x, z)
				if bool(g.get("walkable", false)):
					y = float(g.get("height", y))
			node.position = Vector3(x, y, z)
	# Keep the selection ring glued to the (moving) target each frame (#496).
	if _target_ring != null and _target_ring.visible:
		_position_target_ring()


# --- Server-event signal handlers --------------------------------------------

func _on_handshake_ok() -> void:
	_conn_text = "in world"
	if _router != null:
		_router.note_handshake_ok()   # past this point a drop is an in-world disconnect
	print("[world] HandshakeOk — entered the world")
	# QST-01 (#433): pull the initial quest-log snapshot now that we are in world (worldd
	# does NOT push it unsolicited — #388 — so the client asks). Populates the tracker.
	if _bus != null:
		_bus.request_quest_log()


func _on_movement_state(state: Dictionary) -> void:
	# Our own authoritative state: learn our guid + reconcile the local prediction.
	var new_guid := int(state.get("entity_guid", _my_guid))
	if new_guid != 0 and new_guid != _my_guid:
		_my_guid = new_guid
		# UI-01 (#431/#471): identify the local player to the event bus so the player frame
		# binds to it. worldd sends the local session no self EntityEnter at spawn, so seed
		# name/level/class from character-select; health/power now arrive IMMEDIATELY at spawn
		# via the #439 self-vitals VITALS_UPDATE worldd pushes at ENTER_WORLD (the frame no
		# longer waits on the first combat/heal delta). The bus merges that onto this seed.
		if _bus != null:
			_bus.seed_identity(_my_guid, String(_character.get("name", "")),
				int(_character.get("level", 0)), int(_character.get("class", 0)))
			_bus.set_local_player(_my_guid)
			# The character sheet's paperdoll needs the local BODY (race/sex/appearance),
			# which — like name/level/class above — only char-select carries (#870). The
			# GEAR it wears is NOT seeded here: that stays the authoritative
			# INVENTORY_SNAPSHOT projection worldd pushes at ENTER_WORLD.
			if _character.has("appearance"):
				_bus.seed_local_appearance(int(_character.get("race", 0)),
					int(_character.get("sex", 0)), _character.get("appearance", {}))
			# CMB-01 (#456/#457/#472): the action bar's known-ability set is NO LONGER
			# greybox-seeded here — worldd pushes the character's REAL KNOWN_ABILITIES
			# (0x3005) at ENTER_WORLD (and re-pushes after a growing TRAINER_LEARN), which
			# _route_cast_frame() routes to bus.publish_known_abilities(). A freshly-created
			# character enters knowing nothing (empty bar) and it grows as it trains.
	_last_server_tick = int(state.get("server_time_ms", _last_server_tick))
	if _mover != null:
		# Wire (Z-UP) -> Godot (Y-UP): y = height (wire z), z = ground (wire y).
		var server_pos := Vector3(
			float(state.get("x", 0.0)),
			float(state.get("z", 0.0)),
			float(state.get("y", 0.0)))
		_mover.reconcile(int(state.get("ack_seq", 0)), server_pos,
			float(state.get("orientation", 0.0)))


func _on_entity_frame(opcode: int, _seq: int, payload: PackedByteArray) -> void:
	var d: Dictionary = _net.decode_entity_frame(opcode, payload)
	var kind := String(d.get("kind", ""))
	if kind.is_empty():
		# Not an entity-relay opcode — try the QUEST/GOSSIP decode seam (QST-01, #433).
		# worldd forwards these S→C frames raw through `entity_frame`; we decode + route
		# them through the SAME event bus the HUD reads (never predicting quest state).
		_route_quest_gossip_frame(opcode, payload)
		return  # non-entity opcode (ClockSync / quest / gossip) or undecodable
	var guid := int(d.get("guid", 0))

	# UI-01 (#431): route ALL server unit state through the event bus first — including
	# our OWN (the HUD reads the local player from the bus). This runs BEFORE the
	# "don't render self as a remote" guard below, so self vitals still reach the frame.
	if _bus != null:
		match kind:
			"enter":
				_bus.publish_entity_enter(guid, d)
			"vitals":
				_bus.publish_vitals_update(guid, d)
			"leave":
				_bus.publish_entity_leave(guid)
			"xp":
				# XP_GAINED (CHR-03, #531): fills the HUD XP bar toward the next level.
				_bus.publish_xp_gained(guid, d)
			"level_up":
				# LEVEL_UP (CHR-03, #531): the level-up presentation + raised unit-frame caps.
				_bus.publish_level_up(guid, d)
	if kind == "equipment_visual":
		_apply_equipment_visuals(guid, d.get("equipment", []))
		return
	if kind == "vitals" or kind == "xp" or kind == "level_up":
		return  # a HUD-only delta — never spawns/moves a node
	if kind == "enter":
		_apply_self_enter_equipment(guid, d)

	if guid == _my_guid and _my_guid != 0:
		return  # never render ourselves as a remote
	_last_server_tick = _client_ms
	match kind:
		"enter":
			_spawn_remote(guid, d)
			_interp.on_entity_enter(guid, d.get("position", Vector3.ZERO),
				float(d.get("orientation", 0.0)), _client_ms)
		"update":
			if not _remote_nodes.has(guid):
				_spawn_remote(guid, d)  # defensive: update before enter
			if bool(d.get("has_position", false)):
				_interp.on_entity_update(guid, d.get("position", Vector3.ZERO),
					float(d.get("orientation", 0.0)), _client_ms)
		"leave":
			_despawn_remote(guid)
			_interp.on_entity_leave(guid)


func _on_disconnected(reason: int, message: String) -> void:
	_conn_text = "disconnected (reason %d: %s)" % [reason, message]
	print("[world] disconnected: reason=%d %s" % [reason, message])
	_handle_connection_event(MeridianWorldConnectRouter.EVENT_DISCONNECTED,
		"%s (reason %d)" % [message, reason] if not message.is_empty() else "reason %d" % reason)


func _on_transport_closed(detail: String) -> void:
	_conn_text = "connection closed (%s)" % detail
	_handle_connection_event(MeridianWorldConnectRouter.EVENT_TRANSPORT_CLOSED, detail)


func _on_connect_failed(detail: String) -> void:
	_conn_text = "connect failed (%s)" % detail
	print("[world] connect failed: %s" % detail)
	_handle_connection_event(MeridianWorldConnectRouter.EVENT_CONNECT_FAILED, detail)


# --- Connect-outcome routing (#301 UX) ---------------------------------------
# Feed a connection-ended/failed event to the pure router and act on its decision:
# a PRE-HandshakeOk failure returns the player to Character Select with the error
# surfaced (never left stranded in an empty world); a post-HandshakeOk drop stays
# in the world scene (the HUD already shows the reason). One-shot — the first
# routing wins, so a burst of failure signals doesn't double-swap the scene.
func _handle_connection_event(kind: String, detail: String) -> void:
	if _router == null or _routed_away:
		return
	var decision: Dictionary = _router.decide(kind, detail)
	if String(decision.get("action", "")) != MeridianWorldConnectRouter.ACTION_TO_CHAR_SELECT:
		return
	_routed_away = true
	var message := String(decision.get("message", "Could not enter the world."))
	print("[world] entering world failed pre-HandshakeOk — returning to Character Select: %s" % message)
	_return_to_char_select(message)


# Tear down the world session and swap back to Character Select, carrying the error
# + the account/roster context so the player lands where they left off (not a blank
# screen). Guarded to only run inside a live SceneTree (a headless instantiation
# test builds this node with no current scene).
func _return_to_char_select(message: String) -> void:
	if _net != null:
		_net.disconnect_from_world()
	if not is_inside_tree():
		return
	var packed: PackedScene = load(CHAR_SELECT_SCENE)
	if packed == null:
		_conn_text = "connect failed — could not reload Character Select"
		return
	var char_select := packed.instantiate()
	var account := String(_session.get("account", ""))
	var roster: Array = _session.get("roster", [])
	char_select.configure(account, roster, _session, message)
	var tree := get_tree()
	tree.root.add_child(char_select)
	tree.current_scene = char_select
	queue_free()


# --- Remote-player nodes ------------------------------------------------------

func _spawn_remote(guid: int, d: Dictionary) -> void:
	if _remote_nodes.has(guid):
		return
	var node := Node3D.new()
	node.name = "Remote_%d" % guid
	var pos: Vector3 = d.get("position", SPAWN)
	node.position = pos

	# The mover's BODY: an AssembledCharacter when the frame carried appearance (②/T4,
	# #541), else the class-colored capsule fallback (spec §6). Same code path both here
	# and for the local player — the ONE assembly seam (_build_entity_body). Flat (no
	# emission) for remotes, which keeps them distinct from the OWNER's glowing capsule.
	var body := _build_entity_body(d, int(d.get("char_class", 0)), false)
	node.add_child(body)

	# The entity's on-screen NAME is now the pooled nameplate (CMB-03, #535), attached below
	# once the node is registered — it shows the server name from ENTITY_ENTER and a health
	# bar, superseding the old debug "guid N" label.

	# Pickable collider for click-to-target (#496): a capsule matching the body, on the
	# DEDICATED target layer (mask 0 — it detects nothing; it is only a RAY target). The
	# entity guid rides as metadata so a click resolves the entity straight from the hit
	# collider. Being on its own layer (not layer 1) means selecting never yanks the
	# camera boom and the capsule is never a movement obstacle.
	var picker := StaticBody3D.new()
	picker.name = "Picker"
	picker.collision_layer = TARGET_PHYS_LAYER
	picker.collision_mask = 0
	picker.set_meta("target_guid", guid)
	var pshape := CollisionShape3D.new()
	var pcap := CapsuleShape3D.new()
	pcap.height = 1.8
	pcap.radius = 0.5   # a touch wider than the 0.35 mesh so the capsule is easy to click
	pshape.shape = pcap
	pshape.position = Vector3(0, 0.9, 0)
	picker.add_child(pshape)
	node.add_child(picker)

	_remotes.add_child(node)
	_remote_nodes[guid] = node

	# Nameplates (CMB-03, #535): float a pooled name+health-bar plate over this remote. Only
	# remotes reach here (the local player is guarded out in _on_entity_frame), so the local
	# player's own nameplate is suppressed for free. Name + baseline health ride ENTITY_ENTER.
	if _nameplates != null:
		_nameplates.attach(guid, node, String(d.get("name", "")),
			int(d.get("health", 0)), int(d.get("max_health", 0)))

	print("[world] remote ENTER guid=%d at %s (%d remotes)" % [guid, pos, _remote_nodes.size()])


# --- Body assembly + capsule fallback (②/T4, #541) ---------------------------
# The ONE assembly seam shared by remotes (_spawn_remote) and the local player
# (_build_player_and_camera). When the source carries appearance (an EntityEnter from a
# player entity, or the char-select character on ENTER_WORLD), build an AssembledCharacter
# — the per-race body + worn gear from the wire ids via MeridianContentDB. Any content
# problem degrades to the class-colored capsule, never a crash (spec §6 / contract ① §9):
#   * no "appearance" key (NPC / pre-#538 server) → capsule (today's path untouched);
#   * assemble() == false (no catalog for the race, unloadable body) → capsule.
# Returns a Node3D named "Body" either way, so the TPS camera's yaw target ("../Body")
# and the chat-bubble host resolve it identically whichever body was built.
func _build_entity_body(d: Dictionary, class_id: int, glow: bool) -> Node3D:
	if d.has("appearance"):
		var assembled = AssembledCharacterScript.new()
		assembled.name = "Body"
		var ok: bool = assembled.assemble(int(d.get("race", 0)), int(d.get("sex", 0)),
			d.get("appearance", {}), d.get("equipment", []))
		if ok:
			return assembled
		# No catalog / unloadable body model (spec §6) → discard and fall back to the capsule.
		assembled.free()
	return _build_capsule_body(class_id, glow)


# The class-colored capsule body (#328) — the pre-②/T4 render path, now the FALLBACK used
# whenever appearance is absent or assembly fails (spec §6). `glow` adds the local-owner
# emission cue (#303) so the player can still tell which body is theirs; remotes stay flat.
func _build_capsule_body(class_id: int, glow: bool) -> MeshInstance3D:
	var body := MeshInstance3D.new()
	body.name = "Body"
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	body.mesh = capsule
	body.position = Vector3(0, 0.9, 0)
	var col := PlayerClassColors.color_for(class_id)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = col
	if glow:
		mat.emission_enabled = true
		mat.emission = col * 0.45
	body.material_override = mat
	return body


# --- Floating combat text (CMB-04, #530) -------------------------------------

# Stand up the pooled floating-number system as a world-space child. Built ONCE in
# _ready(); it preallocates its Label3D pool so a hit never allocates.
func _build_floating_text() -> void:
	_floating_text = FloatingCombatTextScript.new()
	_floating_text.name = "FloatingCombatText"
	add_child(_floating_text)


# --- Nameplates (CMB-03, #535) -----------------------------------------------

# Stand up the pooled nameplate manager as a world-space child. Built ONCE in _ready(); it
# preallocates its plate pool so a spawn reparents an existing plate rather than allocating.
func _build_nameplates() -> void:
	_nameplates = NameplateManagerScript.new()
	_nameplates.name = "Nameplates"
	add_child(_nameplates)


# Track a remote's health on its nameplate from the event bus's entity_vitals_changed seam —
# the SAME merged, server-authoritative vitals record the unit frames read (fired on both the
# ENTITY_ENTER baseline and every VITALS_UPDATE delta). No-op for a guid with no plate (the
# local player, or a pre-spawn enter emit): the manager only tracks attached remotes.
func _on_nameplate_vitals(guid: int, vitals: Dictionary) -> void:
	if _nameplates == null or not _nameplates.has(guid):
		return
	_nameplates.update_vitals(guid, int(vitals.get("health", 0)), int(vitals.get("max_health", 0)))
	_nameplates.update_name(guid, String(vitals.get("name", "")))


# Resolve a guid to the SCENE node that represents it: the local player for our own guid,
# otherwise the remote node from the #496 guid→Node3D map. Returns null when the guid is
# not on screen (e.g. an out-of-AoI unit) so callers skip the visual.
func _node_for_guid(guid: int) -> Node3D:
	if guid == _my_guid and _my_guid != 0:
		return _player
	return _remote_nodes.get(guid, null)


# CAST_RESULT resolved (bus.cast_result_received): float a pooled number over the TARGET.
# Pure presentation — every value comes straight from the server frame (the attack-table
# outcome + the damage/heal amount); the client invents nothing (Principle 1). Skips when
# the target has no on-screen node to anchor to.
func _on_cast_result(result: Dictionary) -> void:
	if _floating_text == null:
		return
	var target_guid := int(result.get("target_guid", 0))
	var node := _node_for_guid(target_guid)
	if node == null:
		return
	_floating_text.spawn_over(node.global_position, int(result.get("amount", 0)),
		int(result.get("outcome", FloatingCombatTextScript.OUTCOME_HIT)),
		bool(result.get("is_heal", false)))


func _despawn_remote(guid: int) -> void:
	if not _remote_nodes.has(guid):
		return
	# Nameplates (CMB-03, #535): recycle the plate BACK to the pool before freeing the entity
	# node, so the pooled plate (a child of `node`) survives the despawn instead of being freed.
	if _nameplates != null:
		_nameplates.recycle(guid)
	var node: Node3D = _remote_nodes[guid]
	if node != null:
		node.queue_free()
	_remote_nodes.erase(guid)
	print("[world] remote LEAVE guid=%d (%d remotes)" % [guid, _remote_nodes.size()])


# --- Targeting: click + tab (CMB-01, #496; combat-presentation epic #23) ------
# Real target selection. LEFT-CLICK raycasts the entity under the cursor and selects it
# (empty space clears); TAB cycles the in-AoI entities nearest-first; ESCAPE clears. Every
# choice flows through the event bus (set_target), which drives BOTH the target unit frame
# (name + health/power from target_vitals, live on VITALS_UPDATE) and the on-screen
# selection ring. Friend/foe filtering + range/LoS gating stay future epic-#23 work — at
# M1 every relayed entity is targetable.
func _input(event: InputEvent) -> void:
	# Click-to-target (#496): mouse events drive selection and MUST be handled before the
	# key-only guard below. A clean left CLICK selects the entity under the cursor (empty
	# space clears); a left DRAG is left for the TPS camera to orbit — so we NEVER mark the
	# LMB event handled and only act on release when there was no drag.
	if event is InputEventMouseButton:
		_handle_target_click(event as InputEventMouseButton)
		return
	if event is InputEventMouseMotion:
		if _lmb_down:
			_lmb_drag_px += (event as InputEventMouseMotion).relative.length()
		return
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	var code := (event as InputEventKey).physical_keycode
	# SOC-01 (#434): while the chat input has focus, the player is TYPING — the LineEdit owns
	# the keys (letters, numbers, WASD), so suppress every gameplay keybind here and let the
	# focused control handle the event. The panel releases focus itself on submit (Enter).
	if _hud != null and _hud.is_chat_input_focused():
		return
	match code:
		KEY_ENTER, KEY_KP_ENTER:
			# Open the chat line for typing (focus the input). The panel sends on the next
			# Enter and releases focus so WASD/number keys work again.
			if _hud != null:
				_hud.focus_chat_input()
			get_viewport().set_input_as_handled()
		KEY_TAB:
			_cycle_target()
			get_viewport().set_input_as_handled()  # TAB targets, does not move UI focus
		KEY_G:
			# QST-01 (#433): open gossip on the current NPC. NPC entities don't spawn at
			# M1 (npc_guid maps 1:1 to the npc_template id), so the giver is the current
			# target guid if set, else the dev default (MERIDIAN_GOSSIP_NPC, quartermaster
			# Bren=27) so the flow is drivable in the greybox client.
			_open_gossip_on_current_npc()
			get_viewport().set_input_as_handled()
		KEY_L:
			if _hud != null:
				_hud.toggle_quest_log()  # toggle the quest log window
			get_viewport().set_input_as_handled()
		KEY_B:
			if _hud != null:
				_hud.toggle_bags()  # toggle the bags/inventory window (ITM-01, #441)
			get_viewport().set_input_as_handled()
		KEY_C:
			# Toggle the character sheet: paperdoll + equipment slots (#870, epic #866).
			# [C] is the genre convention and clashes with nothing here — the movement keys
			# are WASD and the other windows are G/L/B/F.
			if _hud != null:
				_hud.toggle_character_sheet()
			get_viewport().set_input_as_handled()
		KEY_F:
			# ITM-02 (#441): open the loot window on the current NPC/corpse. Corpse entities
			# do not spawn at M1, so the corpse is the current target guid if set, else the
			# dev default (MERIDIAN_LOOT_CORPSE) so the flow is drivable in the greybox client.
			_open_loot_on_current_corpse()
			get_viewport().set_input_as_handled()
		KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9:
			# CMB-01 (#432): action-bar keybinds. Slot index is the number key minus one;
			# the HUD drops an out-of-range index. A press → optimistic GCD + CAST_REQUEST.
			if _hud != null:
				_hud.press_action_slot(code - KEY_1)
			get_viewport().set_input_as_handled()
		KEY_ESCAPE:
			# Escape dismisses the open gossip window FIRST (it is the modal-ish thing on
			# screen); with no window open it CLEARS the current target (#496) — the target
			# frame hides and the selection ring disappears via the bus's target_changed.
			if _bus != null:
				if _bus.gossip_npc() != 0:
					_bus.close_gossip()
				else:
					_bus.set_target(0)
			get_viewport().set_input_as_handled()


# --- Quest / gossip controller (QST-01, #433) --------------------------------
# The world scene is the ONLY place both the bus and the net thread meet: it turns bus
# INTENTS into outbound frames and decoded server frames into bus publishes. The HUD
# windows stay pure views that know only the bus.

# The dev NPC to open gossip on when nothing is targeted (npc_template id; overridable
# with MERIDIAN_GOSSIP_NPC). 27 = quartermaster_bren, the IT-M1 kobold-chain giver.
func _default_gossip_npc() -> int:
	var env := OS.get_environment("MERIDIAN_GOSSIP_NPC")
	return int(env) if env.is_valid_int() and int(env) > 0 else 27


func _open_gossip_on_current_npc() -> void:
	if _bus == null:
		return
	var npc := _bus.target_guid()
	if npc == 0:
		npc = _default_gossip_npc()
	print("[world] GOSSIP_HELLO -> npc=%d" % npc)
	_bus.request_gossip_hello(npc)


# --- NPC dialog auto-close (#851) --------------------------------------------
# While a gossip / vendor / trainer window is open, close it once the player leaves the
# NPC's interaction reach — the same UX Esc gives, driven by distance. The bus tracks the
# open NPC guid per window (gossip_npc / vendor_open_npc / trainer_open_npc); we measure the
# player's HORIZONTAL distance to that NPC's spawned node and, past the close threshold,
# reuse the existing bus close path (which hides the window). No per-frame allocation: this
# is a handful of dictionary lookups and float math over at most three open windows.
func _tick_interaction_autoclose() -> void:
	if _bus == null or _player == null:
		return
	var ppos: Vector3 = _player.position
	if _bus.gossip_npc() != 0 and _npc_dialog_out_of_reach(_bus.gossip_npc(), ppos):
		_bus.close_gossip()
	if _bus.vendor_open_npc() != 0 and _npc_dialog_out_of_reach(_bus.vendor_open_npc(), ppos):
		_bus.close_vendor()
	if _bus.trainer_open_npc() != 0 and _npc_dialog_out_of_reach(_bus.trainer_open_npc(), ppos):
		_bus.close_trainer()


# True iff the NPC named by `guid` is beyond the interaction close threshold from `ppos`.
# When the NPC has NO known world position (not a spawned remote entity — e.g. the dev
# raw-template-id gossip fallback, or an NPC not yet / no longer in AoI) the distance is
# unmeasurable, so we DON'T auto-close — mirroring the server gate, which only enforces
# range when the position is knowable (#842). Distance is planar (height-ignoring), the same
# horizontal_distance the server uses.
func _npc_dialog_out_of_reach(guid: int, ppos: Vector3) -> bool:
	var node: Node3D = _remote_nodes.get(guid)
	if node == null:
		return false
	return DialogAutoclose.should_close(ppos, node.position, DialogAutoclose.CLOSE_M)


# The dev corpse to open loot on when nothing is targeted (overridable with
# MERIDIAN_LOOT_CORPSE). Corpse entities do not spawn at M1, so this drives the greybox
# loot flow against a server-side loot session created by a kill.
func _default_loot_corpse() -> int:
	var env := OS.get_environment("MERIDIAN_LOOT_CORPSE")
	return int(env) if env.is_valid_int() and int(env) > 0 else 1


func _open_loot_on_current_corpse() -> void:
	if _bus == null:
		return
	var corpse := _bus.target_guid()
	if corpse == 0:
		corpse = _default_loot_corpse()
	_bus.request_loot(corpse)


# Decode a raw quest/gossip S→C frame and publish it to the bus (the HUD reads it there).
func _route_quest_gossip_frame(opcode: int, payload: PackedByteArray) -> void:
	if _net == null or _bus == null:
		return
	var q: Dictionary = _net.decode_quest_frame(opcode, payload)
	var kind := String(q.get("kind", ""))
	match kind:
		"gossip_menu":
			_bus.publish_gossip_menu(int(q.get("npc_guid", 0)), q.get("options", []))
		"quest_accept_result":
			_bus.publish_quest_accept_result(int(q.get("quest_id", 0)), int(q.get("status", 0)))
		"quest_progress":
			_bus.publish_quest_progress(q)
		"quest_turn_in_result":
			_bus.publish_quest_turn_in_result(q)
		"quest_log":
			_bus.publish_quest_log(q.get("quests", []))
		"quest_marker_update":
			# worldd's PROACTIVE overhead marker for one NPC (#844/#849) — the source of
			# truth for the billboarded !/? (no longer derived from a GOSSIP_MENU), so it
			# shows on sight before any interaction.
			_bus.publish_quest_marker_update(int(q.get("npc_guid", 0)), int(q.get("marker", 0)))
		_:
			# Not a quest/gossip opcode — try the loot/vendor/trainer decode seam (#441).
			_route_econ_frame(opcode, payload)


# Decode a raw loot/vendor/trainer S→C frame and publish it to the bus (ITM/ECO/NPC, #441).
func _route_econ_frame(opcode: int, payload: PackedByteArray) -> void:
	if _net == null or _bus == null:
		return
	var e: Dictionary = _net.decode_econ_frame(opcode, payload)
	match String(e.get("kind", "")):
		"inventory_snapshot":
			_bus.publish_inventory_snapshot(int(e.get("money", 0)), e.get("items", []),
				int(e.get("backpack_slots", 0)), e.get("equipment", []))
		"equipment_change_result":
			_bus.publish_equipment_change_result(e)
		"vendor_list":
			_bus.publish_vendor_list(int(e.get("vendor_id", 0)), e.get("items", []))
		"loot_response":
			_bus.publish_loot_response(int(e.get("corpse_guid", 0)), int(e.get("status", 0)),
				int(e.get("copper", 0)), e.get("items", []))
		"loot_result":
			_bus.publish_loot_result(e)
		"loot_closed":
			_bus.publish_loot_closed(int(e.get("corpse_guid", 0)))
		"vendor_buy_result":
			_bus.publish_vendor_buy_result(e)
		"vendor_sell_result":
			_bus.publish_vendor_sell_result(e)
		"vendor_buyback_result":
			# The server ECHOES buyback_slot in the result (#453/#471), so the bus drops the
			# repurchased row from the echoed slot — no client-side request correlation.
			_bus.publish_vendor_buyback_result(e)
		"trainer_list":
			_bus.publish_trainer_list(int(e.get("npc_guid", 0)), e.get("entries", []))
		"trainer_learn_result":
			_bus.publish_trainer_learn_result(e)
		_:
			# Not a loot/vendor/trainer opcode — try the combat decode seam (CMB-01, #432).
			_route_cast_frame(opcode, payload)


# Decode a raw combat S→C frame (CAST_START/FAILED/RESULT) and publish it to the bus. The
# bus applies the D-10 optimistic-GCD confirm/rollback + drives the cast bar (CMB-01, #432).
func _route_cast_frame(opcode: int, payload: PackedByteArray) -> void:
	if _net == null or _bus == null:
		return
	var c: Dictionary = _net.decode_cast_frame(opcode, payload)
	var now := Time.get_ticks_msec()
	match String(c.get("kind", "")):
		"known_abilities":
			# KNOWN_ABILITIES (0x3005, #457): the character's REAL learned set + metadata,
			# pushed at ENTER_WORLD and re-pushed after a growing TRAINER_LEARN. Seeds the
			# action bar from the wire (replaces the greybox seed) so the GCD/cast prediction
			# reads each ability's real cast_ms/triggers_gcd (fixes the #456 over-prediction).
			_bus.publish_known_abilities(c.get("abilities", []))
		"cast_start":
			_bus.publish_cast_start(int(c.get("ability_id", 0)), int(c.get("cast_ms", 0)),
				int(c.get("server_time_ms", 0)), now)
		"cast_failed":
			_bus.publish_cast_failed(int(c.get("ability_id", 0)), int(c.get("reason", 0)),
				int(c.get("gcd_remaining_ms", 0)), now)
			print("[world] CAST_FAILED ability=%d reason=%d gcd_rem=%d" % [
				int(c.get("ability_id", 0)), int(c.get("reason", 0)),
				int(c.get("gcd_remaining_ms", 0))])
		"cast_result":
			_bus.publish_cast_result(c, now)
		_:
			# Not a combat opcode — try the chat decode seam (SOC-01, #434).
			_route_chat_frame(opcode, payload)


# Decode a raw chat S→C frame (CHAT_DELIVER / CHAT_REJECTED) and publish it to the bus. The
# bus appends it to the scrollback and, for a SAY/YELL line, emits chat_bubble_requested
# (SOC-01, #434). System lines (sender_guid 0) — GM-command replies + the mute notice — ride
# CHAT_DELIVER and render as System lines through the same path.
func _route_chat_frame(opcode: int, payload: PackedByteArray) -> void:
	if _net == null or _bus == null:
		return
	var m: Dictionary = _net.decode_chat_frame(opcode, payload)
	match String(m.get("kind", "")):
		"chat_deliver":
			_bus.publish_chat_deliver(int(m.get("channel", 0)), int(m.get("sender_guid", 0)),
				String(m.get("sender_name", "")), String(m.get("text", "")))
		"chat_rejected":
			_bus.publish_chat_rejected(int(m.get("channel", 0)), int(m.get("reason", 0)),
				String(m.get("target", "")))
		_:
			# Not a chat opcode — try the death/ghost/resurrect decode seam (CMB-03, #532).
			_route_death_frame(opcode, payload)


# Decode a raw death/ghost/resurrect S→C frame (DEATH_STATE / GHOST_STATE / RESURRECT_RESULT)
# and publish it to the bus (CMB-03, #359/#532). The bus drives the death overlay (dim +
# countdown → greyscale ghost + corpse-run guidance → alive) and, on a RESURRECT_RESULT OK,
# restores the local player's health. Presentation-only — never predicted client-side.
func _route_death_frame(opcode: int, payload: PackedByteArray) -> void:
	if _net == null or _bus == null:
		return
	var s: Dictionary = _net.decode_death_frame(opcode, payload)
	match String(s.get("kind", "")):
		"death":
			_bus.publish_death_state(int(s.get("corpse_guid", 0)),
				s.get("corpse_position", Vector3.ZERO), int(s.get("auto_release_ms", 0)))
			print("[world] DEATH_STATE corpse=%d auto_release_ms=%d" % [
				int(s.get("corpse_guid", 0)), int(s.get("auto_release_ms", 0))])
		"ghost":
			_bus.publish_ghost_state(s.get("graveyard_position", Vector3.ZERO),
				int(s.get("corpse_guid", 0)))
			print("[world] GHOST_STATE corpse=%d" % int(s.get("corpse_guid", 0)))
		"resurrect_result":
			_bus.publish_resurrect_result(int(s.get("status", 0)), int(s.get("health", 0)),
				int(s.get("max_health", 0)))
			print("[world] RESURRECT_RESULT status=%d health=%d/%d" % [
				int(s.get("status", 0)), int(s.get("health", 0)), int(s.get("max_health", 0))])
		_:
			pass  # not a death opcode — end of the decode chain (ClockSync etc.)


# --- Bus intent handlers (send the matching outbound frame) -------------------

# Combat (CMB-01, D-10, #432): a slot press → CAST_REQUEST. `client_time_ms` is the press
# stamp the bus captured (Time.get_ticks_msec()); it rides the frame ClockSync-keyed. The
# server ACCEPTS (CAST_START) / REJECTS (CAST_FAILED) / RESOLVES (CAST_RESULT); those come
# back through `entity_frame` → _route_cast_frame → the bus (never predicted here).
func _on_cast_requested(ability_id: int, target_guid: int, client_time_ms: int) -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_cast_request_frame(ability_id, target_guid, client_time_ms)
	if frame.size() > 0:
		_net.send_bulk(frame)
	print("[world] CAST_REQUEST -> ability=%d target=%d" % [ability_id, target_guid])


# Death loop (CMB-03, #359/#532): the death overlay's Release control → RELEASE_REQUEST (an
# empty C→S intent). The server answers with a GHOST_STATE; the client never predicts release.
func _on_release_requested() -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_release_request_frame()
	if frame.size() > 0:
		_net.send_control(frame)
	print("[world] RELEASE_REQUEST ->")


# Death loop (CMB-03, #359/#532): the corpse-run auto-complete or the Resurrect button →
# RESURRECT_REQUEST (an empty C→S intent). The server answers with a RESURRECT_RESULT (OK +
# health, or a typed refusal). Never predicted client-side.
func _on_resurrect_requested() -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_resurrect_request_frame()
	if frame.size() > 0:
		_net.send_control(frame)
	print("[world] RESURRECT_REQUEST ->")


# Chat (SOC-01, #434): a submitted line → CHAT_MESSAGE. `target` matters only for WHISPER; a
# '.'-prefixed body is a GM command the SERVER intercepts on this same opcode (no client-side
# parsing). The server routes/echoes the line back as a CHAT_DELIVER (or refuses it with a
# CHAT_REJECTED), decoded by _route_chat_frame and published to the bus.
func _on_chat_send_requested(channel: int, target: String, text: String) -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_chat_message_frame(channel, target, text)
	if frame.size() > 0:
		_net.send_bulk(frame)
	print("[world] CHAT_MESSAGE -> channel=%d target='%s' text='%s'" % [channel, target, text])


# Float a chat bubble over the sender's entity for a SAY/YELL line. The sender may be the LOCAL
# player (its own echoed line — bubble over _body) or a remote entity (_remote_nodes[guid]). A
# reused MeridianChatBubble child on the node refreshes in place + restarts its own hide timer.
func _on_chat_bubble_requested(sender_guid: int, _sender_name: String, text: String, channel: int) -> void:
	var host: Node3D = null
	if sender_guid == _my_guid and _my_guid != 0:
		host = _body
	elif _remote_nodes.has(sender_guid):
		host = _remote_nodes[sender_guid]
	if host == null:
		return  # sender not visible in this client's AoI — no bubble to float
	var bubble := host.get_node_or_null("ChatBubble") as MeridianChatBubble
	if bubble == null:
		bubble = ChatBubbleScript.new()
		bubble.name = "ChatBubble"
		host.add_child(bubble)
	bubble.show_message(text, channel)


func _on_gossip_hello_requested(npc_guid: int) -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_gossip_hello_frame(npc_guid)
	if frame.size() > 0:
		_net.send_bulk(frame)


func _on_quest_accept_requested(quest_id: int, giver_guid: int) -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_quest_accept_frame(quest_id, giver_guid)
	if frame.size() > 0:
		_net.send_bulk(frame)
	print("[world] QUEST_ACCEPT -> quest=%d giver=%d" % [quest_id, giver_guid])


func _on_quest_turn_in_requested(quest_id: int, turn_in_guid: int, choice_index: int) -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_quest_turn_in_frame(quest_id, turn_in_guid, choice_index)
	if frame.size() > 0:
		_net.send_bulk(frame)
	print("[world] QUEST_TURN_IN -> quest=%d npc=%d choice=%d" % [quest_id, turn_in_guid, choice_index])


func _on_quest_log_requested() -> void:
	if _net == null:
		return
	var frame: PackedByteArray = _net.build_quest_log_request_frame()
	if frame.size() > 0:
		_net.send_bulk(frame)


# Float a giver indicator (!/?) over an NPC's remote node when one exists. `kind` is the
# server-pushed QuestMarkerKind (#844/#849): AVAILABLE = gold `!`, TURN_IN_READY = lit green
# `?`, TURN_IN_INCOMPLETE = dimmed grey `?`, NONE = no marker. NPC entities do not spawn at
# M1 (npc_guid maps to a template id), so this is a no-op until spawns land (mcc #28) — but
# the seam is wired so the marker appears the moment an NPC does.
func _on_giver_indicator_changed(npc_guid: int, kind: int) -> void:
	var node: Node3D = _remote_nodes.get(npc_guid)
	if node == null:
		return
	var existing := node.get_node_or_null("GiverMarker") as Label3D
	if kind == MeridianEventBusScript.MARKER_NONE:
		if existing != null:
			existing.queue_free()
		return
	# Map the server marker to its glyph + colour (the greyed `?` is the third state, #844).
	var glyph := "!"
	var tint := Color(1.0, 0.85, 0.2)  # gold — a quest is available here
	match kind:
		MeridianEventBusScript.MARKER_TURN_IN_READY:
			glyph = "?"
			tint = Color(0.55, 0.9, 0.4)   # lit green — turn-in ready
		MeridianEventBusScript.MARKER_TURN_IN_INCOMPLETE:
			glyph = "?"
			tint = Color(0.5, 0.5, 0.52)   # dimmed grey — turn in here, objectives not done
	if existing == null:
		existing = Label3D.new()
		existing.name = "GiverMarker"
		# #859: a BIG glyph raised clear of the (now lowered, bar-less) NPC nameplate so it is
		# never covered — pixel_size + font_size set the world height (~1.2 m), y clears the name.
		existing.position = Vector3(0, 2.9, 0)
		existing.pixel_size = 0.012
		existing.font_size = 96
		existing.outline_size = 18
		existing.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		existing.no_depth_test = true
		existing.render_priority = 2  # draw the marker over the nameplate labels
		node.add_child(existing)
	existing.text = glyph
	existing.modulate = tint
	# The server only advertises a marker for a quest/friendly NPC, so this guid IS an NPC:
	# drop its nameplate health bar and lower + fade the name so the glyph above reads clean (#859).
	if _nameplates != null:
		_nameplates.mark_npc(npc_guid)


# --- Loot / vendor / trainer controller (ITM-02/ECO-01/NPC-02, #441) ----------
# Gossip vendor/trainer entries open the sibling windows through the bus; loot/vendor/
# trainer INTENTS become outbound frames (send_bulk). The server's typed results are
# decoded by _route_econ_frame and published back to the bus.

func _on_vendor_entry_selected(npc_guid: int) -> void:
	# The vendor catalog is not on the wire (gap), so the greybox uses the gossip NPC guid
	# as the vendor id — the server validates it (UNKNOWN_VENDOR if it is not a vendor).
	if _bus != null:
		_bus.open_vendor(npc_guid)


func _on_trainer_entry_selected(npc_guid: int) -> void:
	# TRAINER_LIST was pushed with the GOSSIP_MENU; this only reveals the window.
	if _bus != null:
		_bus.open_trainer(npc_guid)


func _on_loot_request_requested(corpse_guid: int) -> void:
	_send_econ(_net.build_loot_request_frame(corpse_guid) if _net != null else PackedByteArray())
	print("[world] LOOT_REQUEST -> corpse=%d" % corpse_guid)


func _on_loot_take_requested(corpse_guid: int, slot: int, money: bool) -> void:
	_send_econ(_net.build_loot_take_frame(corpse_guid, slot, money) if _net != null else PackedByteArray())


func _on_loot_release_requested(corpse_guid: int) -> void:
	_send_econ(_net.build_loot_release_frame(corpse_guid) if _net != null else PackedByteArray())


func _on_vendor_buy_requested(vendor_id: int, item_template_id: int, quantity: int) -> void:
	_send_econ(_net.build_vendor_buy_frame(vendor_id, item_template_id, quantity) if _net != null else PackedByteArray())
	print("[world] VENDOR_BUY -> vendor=%d item=%d x%d" % [vendor_id, item_template_id, quantity])


func _on_vendor_sell_requested(vendor_id: int, backpack_slot: int, quantity: int) -> void:
	_send_econ(_net.build_vendor_sell_frame(vendor_id, backpack_slot, quantity) if _net != null else PackedByteArray())
	print("[world] VENDOR_SELL -> vendor=%d slot=%d x%d" % [vendor_id, backpack_slot, quantity])


func _on_vendor_buyback_requested(buyback_slot: int) -> void:
	# The VENDOR_BUYBACK_RESULT echoes the slot (#453/#471), so no client-side correlation.
	_send_econ(_net.build_vendor_buyback_frame(buyback_slot) if _net != null else PackedByteArray())


func _on_trainer_learn_requested(npc_guid: int, ability_id: int) -> void:
	_send_econ(_net.build_trainer_learn_frame(npc_guid, ability_id) if _net != null else PackedByteArray())
	print("[world] TRAINER_LEARN -> npc=%d ability=%d" % [npc_guid, ability_id])


func _on_equipment_change_requested(action: int, slot: int) -> void:
	_send_econ(_net.build_equipment_change_frame(action, slot) if _net != null else PackedByteArray())
	print("[world] EQUIPMENT_CHANGE -> action=%d slot=%d" % [action, slot])


func _apply_equipment_visuals(guid: int, equipment: Array) -> void:
	var body: Node = null
	if guid == _my_guid and _my_guid != 0:
		body = _body
	elif _remote_nodes.has(guid):
		var remote: Node3D = _remote_nodes[guid]
		body = remote.get_node_or_null("Body")
	if body != null and body.has_method("replace_equipment"):
		body.call("replace_equipment", equipment)


# EntityEnter is the reconnect/AoI source of truth. The local body was assembled
# from the roster seed before this frame arrived, so explicitly replace its equipment
# before the self-as-remote guard discards the rest of the enter-render path.
func _apply_self_enter_equipment(guid: int, enter: Dictionary) -> void:
	if guid == _my_guid and _my_guid != 0:
		_apply_equipment_visuals(guid, enter.get("equipment", []))


# Send an already-built econ frame at bulk priority (guards an empty build / offline net).
func _send_econ(frame: PackedByteArray) -> void:
	if _net != null and frame.size() > 0:
		_net.send_bulk(frame)


func _cycle_target() -> void:
	if _bus == null:
		return
	var guids: Array = _remote_nodes.keys()
	if guids.is_empty():
		_bus.set_target(0)  # nothing to target — clear the frame
		return
	var ppos: Vector3 = _player.position if _player != null else SPAWN
	guids.sort_custom(func(a, b): return _dist2_to(int(a), ppos) < _dist2_to(int(b), ppos))
	var cur := _bus.target_guid()
	var idx := guids.find(cur)
	# Cycle to the next-nearest after the current target; else pick the nearest.
	var next_guid := int(guids[(idx + 1) % guids.size()]) if idx != -1 else int(guids[0])
	_bus.set_target(next_guid)
	print("[world] target -> guid=%d (%d candidates)" % [next_guid, guids.size()])


func _dist2_to(guid: int, ppos: Vector3) -> float:
	var n: Node3D = _remote_nodes.get(guid)
	return ppos.distance_squared_to(n.position) if n != null else INF


# --- Click-to-target (#496) ---------------------------------------------------
# LMB tracks a click-vs-drag so targeting and camera-orbit share the button: a press
# starts tracking; motion accumulates drag pixels; a release with little drag (and not
# started over the HUD) is a CLICK that raycasts + selects. The event is NEVER marked
# handled, so the TPS camera still orbits on a left-drag.
func _handle_target_click(mb: InputEventMouseButton) -> void:
	if mb.button_index != MOUSE_BUTTON_LEFT:
		return
	if mb.pressed:
		_lmb_down = true
		_lmb_drag_px = 0.0
		_lmb_press_screen = get_viewport().get_mouse_position()
		# Remember if the press began over a HUD control (action bar / window / chat) so a
		# click meant for the UI never also retargets or clears the world target.
		_lmb_press_on_ui = get_viewport().gui_get_hovered_control() != null
		return
	if not _lmb_down:
		return
	_lmb_down = false
	if _lmb_drag_px > TARGET_CLICK_DRAG_PX:
		return  # a camera-orbit drag, not a select click
	if _lmb_press_on_ui or _bus == null:
		return
	var guid := _pick_target_at_screen(_lmb_press_screen)
	_bus.set_target(guid)  # 0 (empty space / local player / non-entity) → clear
	if guid != 0:
		print("[world] click-target -> guid=%d" % guid)


# Raycast the active camera through `screen_pos` and return the guid of the targetable
# entity under it (0 if the ray hits nothing on the target layer).
func _pick_target_at_screen(screen_pos: Vector2) -> int:
	var cam := get_viewport().get_camera_3d()
	if cam == null:
		return 0
	var from := cam.project_ray_origin(screen_pos)
	var to := from + cam.project_ray_normal(screen_pos) * TARGET_PICK_RANGE
	return _raycast_target_guid(from, to)


# Physics-pick along the segment from→to on the TARGET layer; return the hit entity's
# guid (0 if nothing was hit). Split out from the camera projection so it is unit-testable
# headlessly with an explicit ray (see scenes/world/target_verify.gd).
func _raycast_target_guid(from: Vector3, to: Vector3) -> int:
	var space := get_world_3d().direct_space_state
	if space == null:
		return 0
	var query := PhysicsRayQueryParameters3D.create(from, to, TARGET_PHYS_LAYER)
	var hit: Dictionary = space.intersect_ray(query)
	if hit.is_empty():
		return 0
	return _guid_for_collider(hit.get("collider"))


# Resolve the entity guid carried as metadata on a picked collider (0 if absent).
func _guid_for_collider(collider: Object) -> int:
	var node := collider as Node
	if node != null and node.has_meta("target_guid"):
		return int(node.get_meta("target_guid"))
	return 0


# --- Selection ring (#496) ----------------------------------------------------
# One reusable flat ring, a child of the world, moved onto the current target each frame.
# Shown only for a REMOTE target that is in AoI (its node exists); hidden otherwise.
func _build_target_ring() -> void:
	_target_ring = MeshInstance3D.new()
	_target_ring.name = "TargetRing"
	var torus := TorusMesh.new()
	torus.inner_radius = 0.55
	torus.outer_radius = 0.78
	_target_ring.mesh = torus
	var mat := StandardMaterial3D.new()
	var ring_col := Color(1.0, 0.85, 0.2)
	mat.albedo_color = ring_col
	mat.emission_enabled = true
	mat.emission = ring_col * 0.6
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_target_ring.material_override = mat
	_target_ring.visible = false
	add_child(_target_ring)


func _on_target_changed(guid: int) -> void:
	# Show the ring only for an in-AoI remote target; the per-frame _position_target_ring()
	# keeps it under the (moving) target and hides it if the entity is gone.
	if _target_ring == null:
		return
	if guid != 0 and _remote_nodes.has(guid):
		_position_target_ring()
	else:
		_target_ring.visible = false


func _position_target_ring() -> void:
	if _target_ring == null or _bus == null:
		return
	var node: Node3D = _remote_nodes.get(_bus.target_guid())
	if node == null:
		_target_ring.visible = false
		return
	_target_ring.visible = true
	# Sit the ring at the target's feet (a hair above the ground plane so it never z-fights).
	_target_ring.position = node.position + Vector3(0.0, 0.06, 0.0)


# --- Scene construction (built in code, like camera_demo.gd) -----------------

func _build_environment() -> void:
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-55.0), deg_to_rad(35.0), 0.0)
	add_child(light)

	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.30, 0.40, 0.52)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.45, 0.45, 0.5)
	env.ambient_light_energy = 0.7
	we.environment = env
	add_child(we)


# --- Enter-world terrain streaming (WLD-01, Story E, #558) --------------------
# The full chain wired end to end: a fail-closed pack verify (A/#554) gates entry;
# a MeridianChunkStream (B/#555, C/#556) becomes the chunk root fed the PREDICTED
# player position; the mover's ground sample switches to the per-chunk heightfield
# (D/#557). The loading screen holds until the spawn chunks are resident, then
# reveals; seamless thereafter (client-prd §M1). When no zone pack is configured or
# present (today's default, until real Zone-01 content ships — Forge #26/#315), the
# scene falls back to the M0 flat bootstrap so the networked demo keeps working.

func _build_loading_screen() -> void:
	_loading_screen = WorldLoadingScreenScript.new()
	_loading_screen.name = "LoadingScreen"
	add_child(_loading_screen)


# Enter a streamed zone when a pack is configured/present, else the flat bootstrap.
func _build_world_terrain() -> void:
	var zone := _resolve_zone_paths()
	var chunks_json := String(zone.get("chunks", ""))
	var assets_json := String(zone.get("assets", ""))
	if chunks_json.is_empty() or assets_json.is_empty() \
			or not FileAccess.file_exists(chunks_json) or not FileAccess.file_exists(assets_json):
		# No zone pack -> the M0 flat bootstrap box + landmark grid (still the default for
		# a theme that ships no terrain — e.g. core, until Forge #26/#315 land real
		# Zone-01 content). The mover keeps its FlatWorldQuery backend.
		var where := String(zone.get("dir", ""))
		if where.is_empty():
			where = "theme '%s' (no staged zone)" % MeridianContentDB.resolve_theme()
		print("[world] no zone pack at %s — flat bootstrap" % where)
		_build_ground()
		_build_landmarks()
		return
	var spawn: Vector3 = zone.get("spawn", SPAWN)
	var res: Dictionary = enter_streamed_zone(chunks_json, assets_json, spawn,
		String(zone.get("id", "")))
	if not bool(res.get("ok", false)):
		# Fail-closed (A/#554): a present-but-broken pack must NEVER drop the player onto
		# a guessed map. Surface the reason; build NO terrain and do NOT spawn on it.
		var reason := String(res.get("reason", "chunk pack verify failed"))
		_conn_text = "zone verify failed: %s" % reason
		push_error("[world] ENTER-WORLD BLOCKED (fail-closed) — %s" % reason)
		print("[world] ENTER-WORLD BLOCKED (fail-closed): %s" % reason)


# Resolve the zone manifest paths, in priority: the session handoff (production,
# future), env overrides (dev / headless verify), then the realm THEME's staged zone
# (#877). Returns { "dir", "id", "chunks", "assets"[, "spawn": Vector3] }; "dir" is
# empty when no zone is configured/present at all, which _build_world_terrain reads as
# "flat bootstrap" (the pre-#877 default for a theme that ships no terrain).
func _resolve_zone_paths() -> Dictionary:
	var zsession: Dictionary = _session.get("zone", {}) if _session.get("zone", null) is Dictionary else {}
	var dir := String(zsession.get("dir", ""))
	var id := String(zsession.get("id", ""))
	if dir.is_empty():
		dir = OS.get_environment("MERIDIAN_ZONE_DIR")
	if id.is_empty():
		id = OS.get_environment("MERIDIAN_ZONE_ID")
	# Neither the (future) session handoff nor a dev override named a zone: fall back
	# to whatever zone the realm's theme actually ships.
	if dir.is_empty():
		var found := _discover_theme_zone()
		dir = String(found.get("dir", ""))
		if id.is_empty():
			id = String(found.get("id", ""))
	dir = dir.trim_suffix("/")
	if dir.is_empty() or id.is_empty():
		return {"dir": dir, "id": id}
	var out := {
		"dir": dir,
		"id": id,
		"chunks": "%s/%s.chunks.json" % [dir, id],
		"assets": "%s/%s.assets.json" % [dir, id],
	}
	if zsession.get("spawn", null) is Vector3:
		out["spawn"] = zsession.get("spawn")
	return out


# The zone the realm's THEME ships, discovered under res://meridian/<theme>/chunks/
# (#877). Returns { "dir", "id" } or an empty dict when the theme ships no terrain.
#
# A zone is a subdirectory holding `<name>.chunks.json`, so the zone id comes from the
# staged content itself — chibi's `sprout_meadow` is never spelled in client code, and
# a future theme's zone needs no client change.
#
# Ambiguity is NOT guessed: a theme shipping several zones cannot be resolved without
# the session handoff that names the one the player is entering (production, future).
# Picking one would risk dropping the player onto the WRONG map — the same class of
# error the fail-closed mount exists to prevent — so it degrades to the flat bootstrap
# and says why. Every theme ships exactly one zone today.
func _discover_theme_zone() -> Dictionary:
	var theme := MeridianContentDB.resolve_theme()
	var root := "%s/%s/%s" % [MeridianContentDB.PACK_MOUNT_ROOT, theme, ZONE_CHUNKS_SUBDIR]
	if not DirAccess.dir_exists_absolute(root):
		return {}
	var found: Array[Dictionary] = []
	for zone_dir in DirAccess.get_directories_at(root):
		var dir := "%s/%s" % [root, zone_dir]
		if FileAccess.file_exists("%s/%s.chunks.json" % [dir, zone_dir]):
			found.append({"dir": dir, "id": zone_dir})
	if found.is_empty():
		return {}
	if found.size() > 1:
		var ids := []
		for f in found:
			ids.append(f.get("id", ""))
		ids.sort()
		push_warning("[world] theme '%s' ships %d zones (%s) — cannot pick one without a session zone handoff; set MERIDIAN_ZONE_DIR/MERIDIAN_ZONE_ID. Falling back to the flat bootstrap." % [theme, found.size(), ", ".join(ids)])
		return {}
	return found[0]


# The reusable enter-a-streamed-zone seam (the production _ready path AND the headless
# verify drive it). Fail-closed: it verifies the chunk pack FIRST (A/#554) and refuses
# to build anything on a hard-fail. Returns { "ok", "reason", "verdict" }.
func enter_streamed_zone(chunks_json: String, assets_json: String, spawn: Vector3,
		zone_id: String = "") -> Dictionary:
	# 1. Fail-closed completeness + integrity gate (Story A, #554). BLOCK on any hard-fail.
	var mount := MeridianPackMount.new()
	var verdict: Dictionary = mount.verify_chunk_index(chunks_json, assets_json)
	if not bool(verdict.get("ok", false)):
		return {"ok": false, "reason": String(verdict.get("reason", "chunk pack verify failed")),
			"verdict": int(verdict.get("verdict", -1))}

	# 2. Parse the IF-6 chunk manifest for the zone grid geometry + resolve each cell's
	# server `.chunk.bin` heightfield path (via the IF-8 asset table).
	var geom := _parse_zone_manifest(chunks_json, assets_json)
	if not bool(geom.get("ok", false)):
		return {"ok": false, "reason": String(geom.get("reason", "could not parse zone manifest")),
			"verdict": int(verdict.get("verdict", 0))}

	_zone_origin = Vector3(float(geom.get("origin_x", 0.0)), 0.0, float(geom.get("origin_z", 0.0)))
	_zone_chunk_size = float(geom.get("chunk_size_m", 128.0))
	_zone_server_paths = geom.get("server_paths", {})
	# Fall back to the manifest's own zone id when the caller passed none (the
	# manifest is the authority on what zone these chunks are).
	_zone_id = zone_id if not zone_id.is_empty() else String(geom.get("zone", ""))
	_zone_spawn = spawn

	# 3. Stand up the streamer as the CHUNK ROOT (replaces _build_ground/_build_landmarks).
	_chunk_stream = MeridianChunkStream.new()
	_chunk_stream.name = "ChunkStream"
	if not _chunk_stream.load_zone(chunks_json, assets_json):
		_chunk_stream.free()
		_chunk_stream = null
		return {"ok": false, "reason": "streamer load_zone failed to resolve the manifest",
			"verdict": int(verdict.get("verdict", 0))}
	_chunk_stream.set_instancing_budget(ZONE_INSTANCE_BUDGET)
	_chunk_stream.set_player_position(_zone_spawn)
	add_child(_chunk_stream)

	_zone_active = true
	_zone_loading = true
	_hf_loaded_cells.clear()
	if _loading_screen != null:
		_loading_screen.begin(_zone_id)

	# 4. Wire the mover's ground backend now if it already exists (the verify enters
	# AFTER _ready); the _ready path applies it right after the mover is created.
	if _mover != null:
		_apply_zone_to_mover()
	print("[world] entered streamed zone '%s' (%d chunks, origin (%.1f,%.1f), %.0f m cells)"
		% [_zone_id, int(geom.get("chunk_count", 0)), _zone_origin.x, _zone_origin.z, _zone_chunk_size])
	return {"ok": true, "reason": "", "verdict": int(verdict.get("verdict", 0))}


# Read + parse the IF-6 chunk manifest and IF-8 asset table into the zone geometry
# plus a Vector2i(cx,cz) -> server `.chunk.bin` resource-path map. { "ok", ["reason"] }.
func _parse_zone_manifest(chunks_json: String, assets_json: String) -> Dictionary:
	var cf := FileAccess.get_file_as_string(chunks_json)
	var af := FileAccess.get_file_as_string(assets_json)
	if cf.is_empty() or af.is_empty():
		return {"ok": false, "reason": "empty chunk manifest / asset table"}
	var chunks: Variant = JSON.parse_string(cf)
	var assets: Variant = JSON.parse_string(af)
	if not (chunks is Dictionary) or not (assets is Dictionary):
		return {"ok": false, "reason": "chunk manifest / asset table is not JSON"}

	# id -> resource path (IF-8 asset table).
	var id_to_res: Dictionary = {}
	for e in assets.get("entries", []):
		if e is Dictionary:
			id_to_res[String(e.get("id", ""))] = String(e.get("resource", ""))

	var origin: Dictionary = chunks.get("origin", {})
	var server_paths: Dictionary = {}
	var count := 0
	for c in chunks.get("chunks", []):
		if not (c is Dictionary):
			continue
		count += 1
		var cx := int(c.get("cx", 0))
		var cz := int(c.get("cz", 0))
		var server_ref := String(c.get("server", ""))
		var res_path := String(id_to_res.get(server_ref, ""))
		if not res_path.is_empty():
			server_paths[Vector2i(cx, cz)] = res_path
	return {
		"ok": true,
		"zone": String(chunks.get("zone", "")),
		"origin_x": float(origin.get("x", 0.0)),
		"origin_z": float(origin.get("z", 0.0)),
		"chunk_size_m": float(chunks.get("chunk_size_m", 128.0)),
		"chunk_count": count,
		"server_paths": server_paths,
	}


# Swap the mover's ground query to the heightfield backend over the active zone, then
# seed the spawn. No-op unless a zone is active and the mover exists.
func _apply_zone_to_mover() -> void:
	if not _zone_active or _mover == null:
		return
	_mover.use_heightfield_zone(_zone_origin.x, _zone_origin.z, _zone_chunk_size)
	# Seed the mover at the spawn XZ. The Y is corrected to the real terrain height once
	# the spawn chunk's heightfield is resident (in _tick_world_stream); until then the
	# mover HOLDS (never drops through) via the non-resident-ground guard (#557/#558).
	_mover.reset(_zone_spawn, 0.0)
	if _player != null:
		_player.position = _mover.get_render_position()


# Per-frame streaming drive (Story E). Feed the streamer the predicted player position,
# instance up to the hitch-gated budget, keep the movement heightfield resident WITH the
# render residency (so a step onto a fresh chunk never precedes its ground), and reveal
# the world once the spawn chunks are in.
func _tick_world_stream() -> void:
	if _chunk_stream == null:
		return
	var ppos: Vector3 = _player.position if _player != null else _zone_spawn
	_chunk_stream.set_player_position(ppos)
	_chunk_stream.tick()   # instances up to the configured budget (Story C hitch gate)
	_ensure_heightfield_for_desired()
	if _zone_loading:
		_try_reveal_world()


# Load the shipped `.chunk.bin` heightfield for every DESIRED cell not yet resident on
# the mover, so the ground under the player (and the ring around it) is authoritative
# before the player can walk onto it — the no-fall-through guarantee (#558).
func _ensure_heightfield_for_desired() -> void:
	if _mover == null or not _mover.is_heightfield_active():
		return
	for cell in _chunk_stream.get_desired_cells():
		var v := cell as Vector2i
		if _hf_loaded_cells.has(v):
			continue
		var path: String = _zone_server_paths.get(v, "")
		if path.is_empty() or not FileAccess.file_exists(path):
			continue
		var bytes := FileAccess.get_file_as_bytes(path)
		if bytes.size() > 0 and _mover.add_heightfield_chunk(v.x, v.y, bytes):
			_hf_loaded_cells[v] = true


# Reveal the world once the cell under the spawn is INSTANCED (mesh resident) and its
# heightfield is loaded, standing the player ON the terrain (sampled height, not y=0)
# the instant the world appears. Idempotent — runs only while _zone_loading.
func _try_reveal_world() -> void:
	var cell: Vector2i = _chunk_stream.world_to_cell(_zone_spawn)
	if _loading_screen != null:
		_loading_screen.set_progress("%d chunks resident" % _chunk_stream.get_instanced_count())
	var resident: bool = _chunk_stream.state_at(cell.x, cell.y) == MeridianChunkStream.STATE_INSTANCED
	if not (resident and _hf_loaded_cells.has(cell)):
		return
	# Stand the player ON the terrain (heightfield height) before revealing.
	if _mover != null:
		var g: Dictionary = _mover.sample_ground(_zone_spawn.x, _zone_spawn.z)
		if bool(g.get("walkable", false)):
			_mover.reset(Vector3(_zone_spawn.x, float(g.get("height", 0.0)), _zone_spawn.z), 0.0)
			if _player != null:
				_player.position = _mover.get_render_position()
	_zone_loading = false
	if _loading_screen != null:
		_loading_screen.finish()
	print("[world] spawn chunks resident — world revealed (player stands on terrain)")


# --- Terrain accessors (headless verify / diagnostics) ------------------------
func is_zone_active() -> bool:
	return _zone_active

func is_zone_loading() -> bool:
	return _zone_loading

func get_chunk_stream() -> MeridianChunkStream:
	return _chunk_stream


func _build_ground() -> void:
	# A wide flat ground centred on the spawn (D-19 flat bootstrap plane at y=0).
	var ground := StaticBody3D.new()
	ground.name = "Ground"
	var col := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(256, 1, 256)
	col.shape = box
	ground.add_child(col)
	var mesh := MeshInstance3D.new()
	var pm := BoxMesh.new()
	pm.size = Vector3(256, 1, 256)
	mesh.mesh = pm
	ground.add_child(mesh)
	ground.position = Vector3(SPAWN.x, -0.5, SPAWN.z)
	add_child(ground)


# --- Reference geometry (#303 aid) -------------------------------------------
# Static, brightly-coloured landmarks at KNOWN spawn-relative coordinates plus a
# subtle ground grid. PURELY VISUAL — plain MeshInstance3D with no physics body,
# so they never touch the net/movement/camera-collision path. Their only job is
# to give the eye a fixed reference frame: with fixed posts + grid lines around
# spawn, even a metre or two of local-player motion is obvious by parallax, so
# "am I moving, or is that just the bot?" is answerable at a glance (the whole
# reason WASD "felt" dead on a featureless plane, #303). Post labels carry the
# WIRE-frame coordinate (x = ground east, y = ground north) so they line up with
# the server MovementState / move-debug the owner reads.
func _build_landmarks() -> void:
	_build_ground_grid()

	# Cardinal cross around spawn + scattered corner posts, all SPAWN-RELATIVE so they
	# follow the enter origin (now the -320 Zone-01 centre, #805) instead of the old
	# hardcoded (64,64). Each row is (offset_east, offset_north, colour, dir) in metres
	# from spawn; Godot maps wire (x, y) -> (x, z) with y = height. The tag carries the
	# resolved ABSOLUTE wire coordinate so the posts still line up with the server
	# MovementState / move-debug the owner reads.
	var sx := SPAWN.x   # wire east  (Godot x)
	var sy := SPAWN.z   # wire north (Godot z)
	var posts := [
		[-10.0, 0.0, Color(0.90, 0.25, 0.25), "W"],
		[10.0, 0.0, Color(0.95, 0.55, 0.15), "E"],
		[0.0, -10.0, Color(0.95, 0.82, 0.20), "S"],
		[0.0, 10.0, Color(0.35, 0.55, 0.95), "N"],
		[-20.0, -20.0, Color(0.80, 0.35, 0.85), "SW"],
		[20.0, 20.0, Color(0.35, 0.80, 0.85), "NE"],
		[-20.0, 20.0, Color(0.55, 0.35, 0.90), "NW"],
		[20.0, -20.0, Color(0.85, 0.45, 0.55), "SE"],
	]
	for p in posts:
		var wx: float = sx + float(p[0])
		var wy: float = sy + float(p[1])
		_add_pillar(wx, wy, p[2] as Color, "%s (%d,%d)" % [String(p[3]), int(wx), int(wy)])


func _add_pillar(wire_x: float, wire_y: float, colour: Color, tag: String) -> void:
	var pillar := MeshInstance3D.new()
	pillar.name = "Landmark_%s" % tag
	var bm := BoxMesh.new()
	bm.size = Vector3(1.5, 6.0, 1.5)
	pillar.mesh = bm
	var mat := StandardMaterial3D.new()
	mat.albedo_color = colour
	mat.emission_enabled = true            # a gentle glow so posts read at distance
	mat.emission = colour * 0.35
	pillar.material_override = mat
	# Godot: wire x -> x, wire y -> z. Sit the 6 m pillar on the ground (base y=0).
	pillar.position = Vector3(wire_x, 3.0, wire_y)

	var label := Label3D.new()
	label.text = tag
	label.position = Vector3(0, 4.0, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.modulate = colour
	pillar.add_child(label)
	add_child(pillar)


func _build_ground_grid() -> void:
	# Thin dark stripes every 4 m across a 64 m span centred on spawn. Spaced box
	# meshes (no shader / no asset) so any lateral motion visibly slides the
	# capsule across the lines. Lifted a hair above the ground plane (y ~= 0.02).
	var grid := Node3D.new()
	grid.name = "GroundGrid"
	var span := 64.0
	var step := 4.0
	var half := span / 2.0
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.16, 0.20, 0.26)
	var n := int(span / step)
	for i in range(n + 1):
		var off := -half + float(i) * step
		# Stripe running along Z (wire-north) at x = spawn.x + off.
		var lx := MeshInstance3D.new()
		var bx := BoxMesh.new()
		bx.size = Vector3(0.08, 0.02, span)
		lx.mesh = bx
		lx.material_override = mat
		lx.position = Vector3(SPAWN.x + off, 0.02, SPAWN.z)
		grid.add_child(lx)
		# Stripe running along X (wire-east) at z = spawn.z + off.
		var lz := MeshInstance3D.new()
		var bz := BoxMesh.new()
		bz.size = Vector3(span, 0.02, 0.08)
		lz.mesh = bz
		lz.material_override = mat
		lz.position = Vector3(SPAWN.x, 0.02, SPAWN.z + off)
		grid.add_child(lz)
	add_child(grid)


func _build_player_and_camera() -> void:
	_player = Node3D.new()
	_player.name = "Player"
	_player.position = SPAWN
	add_child(_player)

	# The LOCAL player body: an AssembledCharacter seeded from the char-select character
	# when it carries appearance (②/T4, #541), else the class-colored capsule — the SAME
	# assembly seam remotes use (_build_entity_body). Seeded from _character (race +
	# appearance + class from ENTER_WORLD); local equipment isn't on the char row yet
	# (M1), so the local body assembles body + presets and equips nothing. The owner still
	# tells which body is theirs by the emission GLOW on the capsule fallback (remotes are
	# flat) plus the floating "YOU" label — the #303 "which body am I?" cue is preserved.
	# A capsule fallback stays class-colored so a second client sees a consistent color.
	var local_class := int(_character.get("class", 0))
	var seed := {
		"char_class": local_class,
		"race": int(_character.get("race", 0)),
	}
	if _character.has("appearance"):
		seed["appearance"] = _character["appearance"]
		seed["sex"] = int(_character.get("sex", 0))
		seed["equipment"] = _character.get("equipment", [])
	_body = _build_entity_body(seed, local_class, true)
	var nose := MeshInstance3D.new()
	var nm := BoxMesh.new()
	nm.size = Vector3(0.2, 0.2, 0.5)
	nose.mesh = nm
	nose.position = Vector3(0, 0.2, -0.5)
	_body.add_child(nose)
	# A big floating "YOU" tag (green, to match the capsule), distinct from the
	# remote "guid …" labels. Keeps the character name when one is known.
	var who := String(_character.get("name", ""))
	var name_label := Label3D.new()
	name_label.text = "YOU" if who.is_empty() else "YOU — %s" % who
	name_label.modulate = Color(0.25, 1.0, 0.45)
	name_label.font_size = 48
	name_label.outline_size = 12
	name_label.position = Vector3(0, 1.6, 0)
	name_label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	name_label.no_depth_test = true
	_body.add_child(name_label)
	_player.add_child(_body)

	_camera = ClassDB.instantiate("MeridianTpsCamera") as Node3D
	_camera.name = "TpsCamera"
	_camera.set("yaw_target_path", NodePath("../Body"))
	_player.add_child(_camera)


func _build_hud() -> void:
	var layer := CanvasLayer.new()
	layer.name = "HUD"
	add_child(layer)
	var panel := VBoxContainer.new()
	panel.position = Vector2(12, 12)
	layer.add_child(panel)
	_hud_state = _hud_line(panel)
	_hud_guid = _hud_line(panel)
	_hud_remotes = _hud_line(panel)
	_hud_tick = _hud_line(panel)


func _hud_line(parent: Control) -> Label:
	var l := Label.new()
	l.add_theme_color_override("font_color", Color.WHITE)
	l.add_theme_color_override("font_outline_color", Color.BLACK)
	l.add_theme_constant_override("outline_size", 4)
	parent.add_child(l)
	return l


func _refresh_hud() -> void:
	if _hud_state == null:
		return
	_hud_state.text = "connection: %s" % _conn_text
	_hud_guid.text = "my guid: %s" % ("%d" % _my_guid if _my_guid != 0 else "—")
	_hud_remotes.text = "remote entities: %d" % _remote_nodes.size()
	_hud_tick.text = "last server tick: %d ms" % _last_server_tick
