# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — minimal login screen (issue #99, PRD CHR-01 / login rows).
#
# The visual login screen: host / account / password fields, a Login button, and a
# status line. It is DELIBERATELY minimal — the #99 deliverable is the FLOW + the
# net core (LoginFlow + the MeridianLogin GDExtension), not pixel-perfect UI. A
# later UI story (Client SAD §2.3 login scene) skins this; the wiring here is the
# contract that story fills in.
#
# It attaches a LoginFlow, feeds it the field values, disables the form while a
# login runs, and on success prints/records the grant + realm then hands off to the
# world connect (build_world_hello_frame). Errors are surfaced on the status line,
# localizable by the MeridianLogin.STATUS_* code (bad credentials, connect failure,
# realm unavailable, …).
#
# Scene shape (login_screen.tscn — a plain Control tree):
#   Control (this script)
#     └ VBox
#         ├ HostEdit      (LineEdit)   default "127.0.0.1"
#         ├ PortEdit      (LineEdit)   default "7100"
#         ├ AccountEdit   (LineEdit)
#         ├ PasswordEdit  (LineEdit, secret)
#         ├ LoginButton   (Button)
#         └ StatusLabel   (Label)

extends Control

# The char-select scene the login hands off to on success (boot flow:
# Boot/Login → CharSelect → World, issue #110). Instantiated + configured with the
# logged-in account before it replaces this scene.
const CHAR_SELECT_SCENE: String = "res://scenes/charselect/char_select.tscn"

@onready var _host: LineEdit = %HostEdit
@onready var _port: LineEdit = %PortEdit
@onready var _account: LineEdit = %AccountEdit
@onready var _password: LineEdit = %PasswordEdit
@onready var _login_button: Button = %LoginButton
@onready var _status: Label = %StatusLabel

var _flow: LoginFlow


func _ready() -> void:
	_flow = LoginFlow.new()
	_flow.login_succeeded.connect(_on_login_succeeded)
	_flow.login_failed.connect(_on_login_failed)
	_login_button.pressed.connect(_on_login_pressed)
	_set_status("Enter your account and press Login.")


func _on_login_pressed() -> void:
	var host := _host.text.strip_edges()
	var port := int(_port.text.strip_edges())
	var account := _account.text.strip_edges()
	var password := _password.text  # never strip a password
	if host.is_empty() or account.is_empty() or password.is_empty():
		_set_status("Host, account and password are required.")
		return
	_set_form_enabled(false)
	_set_status("Connecting to %s:%d …" % [host, port])
	# realm_id 0 => auto-select the first build-compatible realm (M0: one realm).
	_flow.begin_login(host, port, account, password, 0)


func _on_login_succeeded(result: Dictionary) -> void:
	_set_form_enabled(true)
	var realm_id: int = int(result.get("realm_id", 0))
	var tls: String = String(result.get("tls_version", ""))
	var realms: Array = result.get("realms", [])
	var realm_name := ""
	for r in realms:
		if int(r.get("id", -1)) == realm_id:
			realm_name = String(r.get("name", ""))
	_set_status("Logged in over %s. Realm '%s' (id %d). Entering world…" % [tls, realm_name, realm_id])

	# Hand off to the IF-2 world handshake: the WorldHello frame the client sends
	# to worldd. The world-connect code (a later net story) opens the realm socket
	# and writes this first. Here we just prove the grant produced a valid frame.
	var world_hello := _flow.build_world_hello_frame()
	print("[login] grant_id=%d session_key=%d bytes world_hello=%d bytes → IF-2 kickoff"
		% [_flow.grant_id(), _flow.session_key().size(), world_hello.size()])

	# Advance the boot flow: Login → CharSelect (issue #110). The char-select screen
	# lists this account's characters and, on Enter World, hands off to the world map.
	_go_to_char_select(_account.text.strip_edges())


# Replace this scene with the character-select screen, seeding it with the logged-in
# account. Configured BEFORE it enters the tree so its _ready() shows the right account.
func _go_to_char_select(account: String) -> void:
	var packed: PackedScene = load(CHAR_SELECT_SCENE)
	if packed == null:
		_set_status("Could not load character select.")
		return
	var char_select := packed.instantiate()
	char_select.configure(account)
	var tree := get_tree()
	tree.root.add_child(char_select)
	tree.current_scene = char_select
	queue_free()


func _on_login_failed(status: int, detail: String) -> void:
	_set_form_enabled(true)
	# Localize by code (M0: pass the C++ status name through as a fallback). A real
	# UI maps STATUS_* to translated strings; the detail is the fallback.
	var human := _message_for_status(status)
	_set_status("Login failed: %s (%s)" % [human, detail])


func _message_for_status(status: int) -> String:
	match status:
		MeridianLogin.STATUS_BAD_CREDENTIALS:
			return "incorrect account or password"
		MeridianLogin.STATUS_CONNECT_FAILED:
			return "could not reach the login server"
		MeridianLogin.STATUS_PROTOCOL_MISMATCH:
			return "client out of date — please update"
		MeridianLogin.STATUS_REALM_UNAVAILABLE:
			return "that realm is unavailable"
		MeridianLogin.STATUS_SERVER_PROOF_FAILED:
			return "server could not be verified — do not enter your password"
		_:
			return "connection problem, please try again"


func _set_form_enabled(enabled: bool) -> void:
	_host.editable = enabled
	_port.editable = enabled
	_account.editable = enabled
	_password.editable = enabled
	_login_button.disabled = not enabled


func _set_status(text: String) -> void:
	if _status != null:
		_status.text = text
