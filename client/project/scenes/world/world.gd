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

# The M0 bootstrap spawn: worldd seeds every session at wire (64, 64, 0) — Godot
# (x=64, y=height 0, z=64). The local player + camera start here.
const SPAWN := Vector3(64.0, 0.0, 64.0)

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

# Character Select — where a PRE-HandshakeOk connection failure returns the player
# (#301 UX: never strand them in an empty world). Kept in sync with the char-select
# handoff (scenes/charselect/char_select.gd → WORLD_SCENE).
const CHAR_SELECT_SCENE := "res://scenes/charselect/char_select.tscn"

# The single shared class→color table (#328). BOTH the local player capsule and the
# remote-player capsules resolve their color through this one script, so own vs.
# remote coloring can never drift — every client renders a class the same way.
const PlayerClassColors := preload("res://scenes/world/player_class_colors.gd")

# HUD foundation (UI-01, #431): the MVVM event bus + the unit-frame HUD. world.gd
# owns the ONE event bus for this world session and publishes every decoded server
# unit event into it; the HUD subscribes to the bus and never touches the net thread.
const MeridianEventBusScript := preload("res://hud/event_bus.gd")
const MeridianHudScript := preload("res://hud/hud.gd")
# Floating chat bubble (SOC-01, #434) — attached to an entity node for say/yell lines.
const ChatBubbleScript := preload("res://hud/chat_bubble.gd")

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
	_build_ground()
	_build_landmarks()
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
	# Combat intent (CMB-01, D-10, #432): a slot press → CAST_REQUEST frame.
	_bus.cast_requested.connect(_on_cast_requested)
	# Chat (SOC-01, #434): a submitted line → CHAT_MESSAGE frame; a delivered SAY/YELL line
	# floats a bubble over the sender's entity. CHAT_DELIVER/CHAT_REJECTED are decoded +
	# published back through the SAME bus (never invented client-side — the sender even sees
	# its own say/yell because worldd echoes it).
	_bus.chat_send_requested.connect(_on_chat_send_requested)
	_bus.chat_bubble_requested.connect(_on_chat_bubble_requested)

	_interp = MeridianRemoteInterpolator.new()
	_mover = MeridianMovementController.new()
	_mover.reset(SPAWN, 0.0)

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

	_refresh_hud()


func _tick_local_player() -> void:
	# WASD relative to the camera yaw (physical keys so no input-map dependency).
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
		yaw = _camera.get_character_yaw()
	# Rotate the local (strafe, forward) input into world axes by the facing yaw.
	# Godot forward is -Z; a yaw of 0 faces -Z.
	var sin_y := sin(yaw)
	var cos_y := cos(yaw)
	var world_x := strafe * cos_y - (-fwd) * sin_y
	var world_z := strafe * sin_y + (-fwd) * cos_y
	var move := Vector3(world_x, 0.0, world_z)

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
	for guid in _remote_nodes.keys():
		var node: Node3D = _remote_nodes[guid]
		if node == null:
			continue
		var sample: Dictionary = _interp.sample_entity(int(guid), _client_ms)
		if int(sample.get("kind", 0)) != 0:  # 0 = empty (nothing buffered yet)
			node.position = Vector3(
				float(sample.get("x", 0.0)),
				float(sample.get("y", 0.0)),
				float(sample.get("z", 0.0)))
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
	if kind == "vitals":
		return  # a vitals delta only updates the HUD — it never spawns/moves a node

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

	var body := MeshInstance3D.new()
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	body.mesh = capsule
	body.position = Vector3(0, 0.9, 0)
	# Color the capsule by the mover's class (#328), via the SHARED table the local
	# player uses too — so this remote renders in the SAME color on every client, and
	# in the same color the local player would show for that class. Flat (no emission),
	# which keeps it distinct from the OWNER's glowing local capsule.
	var class_id := int(d.get("char_class", 0))
	var col := PlayerClassColors.color_for(class_id)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = col
	body.material_override = mat
	node.add_child(body)

	var label := Label3D.new()
	label.text = "guid %d" % guid
	label.modulate = col
	label.position = Vector3(0, 2.2, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	node.add_child(label)

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
	print("[world] remote ENTER guid=%d at %s (%d remotes)" % [guid, pos, _remote_nodes.size()])


func _despawn_remote(guid: int) -> void:
	if not _remote_nodes.has(guid):
		return
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
				int(e.get("backpack_slots", 0)))
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
			pass  # not a chat opcode — ignore (ClockSync etc.)


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


# Float a giver indicator (!/?) over an NPC's remote node when one exists. NPC entities
# do not spawn at M1 (npc_guid maps to a template id), so this is a no-op until spawns
# land (mcc #28) — but the seam is wired so the marker appears the moment an NPC does.
func _on_giver_indicator_changed(npc_guid: int, marker: String) -> void:
	var node: Node3D = _remote_nodes.get(npc_guid)
	if node == null:
		return
	var existing := node.get_node_or_null("GiverMarker") as Label3D
	if marker.is_empty():
		if existing != null:
			existing.queue_free()
		return
	if existing == null:
		existing = Label3D.new()
		existing.name = "GiverMarker"
		existing.position = Vector3(0, 2.6, 0)
		existing.font_size = 64
		existing.outline_size = 12
		existing.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		existing.no_depth_test = true
		node.add_child(existing)
	existing.text = marker
	existing.modulate = Color(1.0, 0.85, 0.2) if marker == "!" else Color(0.55, 0.9, 0.4)


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

	# Cardinal cross around the (64,64) spawn + scattered corner posts. Each row
	# is (wire_x, wire_y, colour, tag). Godot maps wire (x, y) -> (x, z); y=height.
	var posts := [
		[54.0, 64.0, Color(0.90, 0.25, 0.25), "W (54,64)"],
		[74.0, 64.0, Color(0.95, 0.55, 0.15), "E (74,64)"],
		[64.0, 54.0, Color(0.95, 0.82, 0.20), "S (64,54)"],
		[64.0, 74.0, Color(0.35, 0.55, 0.95), "N (64,74)"],
		[44.0, 44.0, Color(0.80, 0.35, 0.85), "(44,44)"],
		[84.0, 84.0, Color(0.35, 0.80, 0.85), "(84,84)"],
		[44.0, 84.0, Color(0.55, 0.35, 0.90), "(44,84)"],
		[84.0, 44.0, Color(0.85, 0.45, 0.55), "(84,44)"],
	]
	for p in posts:
		_add_pillar(float(p[0]), float(p[1]), p[2] as Color, String(p[3]))


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

	_body = MeshInstance3D.new()
	_body.name = "Body"
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	(_body as MeshInstance3D).mesh = capsule
	_body.position = Vector3(0, 0.9, 0)
	# The LOCAL player: colored by ITS class (#328) through the SHARED table remotes
	# use too, so a second client sees this same character in the same color. The
	# owner still tells which capsule is theirs by the emission GLOW (remotes are flat)
	# plus the floating "YOU" label — the #303 "which capsule am I?" cue is preserved
	# without a hardcoded color that would disagree with how others see this player.
	var local_class := int(_character.get("class", 0))
	var local_col := PlayerClassColors.color_for(local_class)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = local_col
	mat.emission_enabled = true
	mat.emission = local_col * 0.45
	(_body as MeshInstance3D).material_override = mat
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
