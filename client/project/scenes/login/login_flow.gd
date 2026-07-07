# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — login flow controller (issue #99).
#
# The GDScript half of the IF-1/IF-2 client login flow. The C++ GDExtension class
# MeridianLogin (meridian_login.*) owns ALL the protocol + crypto: it connects to
# authd over TLS 1.3, runs the SRP-6a login, fetches the realm list, selects a
# realm, and returns the single-use SessionGrant. This script is the thin driver
# the Boot / Login scene attaches: it collects host + credentials, runs the login
# OFF the main thread (Client SAD §6.1 — never block the render thread on a socket),
# and surfaces the outcome (grant + realm, or a clear error) to the UI.
#
# The FLOW + the net core (the C++ side) are the #99 deliverable; the visual login
# screen is intentionally minimal (login_screen.gd wires a few controls to this).
# This is the client counterpart to the whole server auth path (authd #79 /
# worldd #84) and the login half of IT-M0.
#
# Usage (from a scene):
#     var flow := LoginFlow.new()
#     flow.login_succeeded.connect(_on_login_ok)     # (result: Dictionary)
#     flow.login_failed.connect(_on_login_err)       # (status: int, detail: String)
#     flow.begin_login("auth.example.com", 7100, "alice", "s3cret", 0)
#
# On success `result` carries: grant_id, session_key (PackedByteArray, 32 B),
# realm_id, reconnect_window_ms, realms (Array), tls_version. The scene then hands
# off to the world connect (IF-2): see build_world_hello_frame() below.

extends RefCounted
class_name LoginFlow

## Emitted on a successful login. `result` is the MeridianLogin.login() Dictionary
## (grant_id, session_key, realm_id, reconnect_window_ms, realms, tls_version).
signal login_succeeded(result: Dictionary)

## Emitted on any failure. `status` is a MeridianLogin.STATUS_* code; `detail` is a
## human-readable message (already localizable by code on the UI side).
signal login_failed(status: int, detail: String)

# The pinned client build + IF-1/IF-2 protocol version. Real values come from the
# build system / ENGINE pin; these defaults match authd's LoginConfig defaults so a
# dev build logs into a dev authd out of the box.
const CLIENT_BUILD: int = 1000
const PROTO_VER: int = 1

var _login: MeridianLogin
var _thread: Thread


func _init() -> void:
	_login = MeridianLogin.new()
	_login.set_client_build(CLIENT_BUILD)
	_login.set_proto_ver(PROTO_VER)


# Kick off a login on a worker thread. Returns immediately; the outcome arrives on
# login_succeeded / login_failed (marshalled back to the main thread via call_deferred
# so the UI can touch the scene tree safely). `realm_id` == 0 auto-selects the first
# build-compatible realm; a non-zero id pins a specific realm.
func begin_login(host: String, port: int, account: String, password: String, realm_id: int = 0) -> void:
	if _thread != null and _thread.is_alive():
		push_warning("LoginFlow: a login is already in progress")
		return
	_thread = Thread.new()
	var args := { "host": host, "port": port, "account": account, "password": password, "realm_id": realm_id }
	_thread.start(_run_login.bind(args))


# Worker-thread body: the blocking login call. MeridianLogin.login() does the whole
# IF-1 exchange over TLS and returns a plain Dictionary — no scene-tree access here.
func _run_login(args: Dictionary) -> void:
	var result: Dictionary = _login.login(
		args.host, args.port, args.account, args.password, args.realm_id
	)
	# Hop back to the main thread to emit signals + join the thread.
	_finish.call_deferred(result)


func _finish(result: Dictionary) -> void:
	if _thread != null:
		_thread.wait_to_finish()
		_thread = null
	if bool(result.get("ok", false)):
		login_succeeded.emit(result)
	else:
		var status: int = int(result.get("status", MeridianLogin.STATUS_PROTOCOL_ERROR))
		var detail: String = String(result.get("detail", "login failed"))
		login_failed.emit(status, detail)


# After a successful login, build the IF-2 WorldHello frame (the FIRST frame the
# client sends to worldd) from the cached grant. The world-connect code sends this
# over its own TCP/TLS connection to the selected realm's address:port. Returns an
# empty PackedByteArray if there is no grant (login not completed).
func build_world_hello_frame() -> PackedByteArray:
	return _login.build_world_hello_frame()


# Convenience accessors for the world-connect step (mirror the cached grant).
func has_grant() -> bool:
	return _login.has_grant()


func grant_id() -> int:
	return _login.get_grant_id()


func session_key() -> PackedByteArray:
	return _login.get_session_key()


func realm_id() -> int:
	return _login.get_realm_id()
