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

# Character Select — where a PRE-HandshakeOk connection failure returns the player
# (#301 UX: never strand them in an empty world). Kept in sync with the char-select
# handoff (scenes/charselect/char_select.gd → WORLD_SCENE).
const CHAR_SELECT_SCENE := "res://scenes/charselect/char_select.tscn"

var _session: Dictionary = {}
var _character: Dictionary = {}

var _net: MeridianNetThread
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

var _hud_state: Label
var _hud_guid: Label
var _hud_remotes: Label
var _hud_tick: Label

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


func _ready() -> void:
	_move_debug = not OS.get_environment("MERIDIAN_MOVE_DEBUG").is_empty()
	if _move_debug:
		print("[world.move] MERIDIAN_MOVE_DEBUG on — logging local-player input/render once per second")
	_build_environment()
	_build_ground()
	_build_player_and_camera()
	_remotes = Node3D.new()
	_remotes.name = "Remotes"
	add_child(_remotes)
	_build_hud()

	_interp = MeridianRemoteInterpolator.new()
	_mover = MeridianMovementController.new()
	_mover.reset(SPAWN, 0.0)

	if _has_session():
		_connect_to_world()
	else:
		_conn_text = "offline (no session — local sandbox)"
		print("[world] no session context — running OFFLINE local sandbox")
	_refresh_hud()


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
	print("[world.move] W=%s A=%s S=%s D=%s yaw=%.3f move=(%.2f,%.2f,%.2f) render=(%.2f,%.2f,%.2f) pred=(%.2f,%.2f,%.2f) last_err=%.3f srv_tick=%d"
		% [key_w, key_a, key_s, key_d, yaw,
			move.x, move.y, move.z,
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


# --- Server-event signal handlers --------------------------------------------

func _on_handshake_ok() -> void:
	_conn_text = "in world"
	if _router != null:
		_router.note_handshake_ok()   # past this point a drop is an in-world disconnect
	print("[world] HandshakeOk — entered the world")


func _on_movement_state(state: Dictionary) -> void:
	# Our own authoritative state: learn our guid + reconcile the local prediction.
	_my_guid = int(state.get("entity_guid", _my_guid))
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
		return  # non-entity opcode (e.g. ClockSync) or undecodable
	var guid := int(d.get("guid", 0))
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
	# A tint so remote players read as distinct from the local capsule.
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.85, 0.4, 0.3)
	body.material_override = mat
	node.add_child(body)

	var label := Label3D.new()
	label.text = "guid %d" % guid
	label.position = Vector3(0, 2.2, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	node.add_child(label)

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
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.3, 0.55, 0.85)
	(_body as MeshInstance3D).material_override = mat
	var nose := MeshInstance3D.new()
	var nm := BoxMesh.new()
	nm.size = Vector3(0.2, 0.2, 0.5)
	nose.mesh = nm
	nose.position = Vector3(0, 0.2, -0.5)
	_body.add_child(nose)
	var name_label := Label3D.new()
	name_label.text = String(_character.get("name", "You"))
	name_label.position = Vector3(0, 1.4, 0)
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
