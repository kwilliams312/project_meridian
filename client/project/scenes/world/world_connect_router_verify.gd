# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless UNIT TEST for the world-connect routing decision
# (issue #301 UX). Runs with NO display, NO server, NO GDExtension:
#   godot --headless --path project --script res://scenes/world/world_connect_router_verify.gd
#
# It drives MeridianWorldConnectRouter (scenes/world/world_connect_router.gd) — the
# pure state machine scenes/world/world.gd uses to decide, on a connection failure,
# whether to bounce the player back to Character Select. The router is engine-free,
# so this test proves the load-bearing UX decision headlessly even though the
# on-screen scene transition needs a real GPU (#283). Exits 0 on all-pass, 1 on any
# failed assertion.

extends SceneTree

const Router := preload("res://scenes/world/world_connect_router.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian world-connect ROUTER unit test (#301 connect-fail UX)")

	# --- 1. PRE-HandshakeOk failures ALL route back to Character Select. ---
	# connect_failed (never reached worldd — e.g. TLS wrong version / socket refused,
	# the exact class of failure that stranded the player before this fix).
	var r1 := Router.new()
	_check("starts not-yet-in-world", not r1.has_entered_world())
	var d1: Dictionary = r1.decide(Router.EVENT_CONNECT_FAILED, "TLS wrong version number")
	_check("pre-handshake connect_failed -> Character Select",
		d1.get("action", "") == Router.ACTION_TO_CHAR_SELECT)
	_check("connect_failed carries the transport detail",
		String(d1.get("message", "")).find("TLS wrong version number") != -1)
	_check("connect_failed message names Character Select",
		String(d1.get("message", "")).find("Character Select") != -1)

	# transport_closed before handshake (peer closed the socket before HandshakeOk).
	var r2 := Router.new()
	var d2: Dictionary = r2.decide(Router.EVENT_TRANSPORT_CLOSED, "peer closed")
	_check("pre-handshake transport_closed -> Character Select",
		d2.get("action", "") == Router.ACTION_TO_CHAR_SELECT)

	# disconnected before handshake (worldd rejected entry, e.g. spent grant).
	var r3 := Router.new()
	var d3: Dictionary = r3.decide(Router.EVENT_DISCONNECTED, "grant already consumed")
	_check("pre-handshake disconnected -> Character Select",
		d3.get("action", "") == Router.ACTION_TO_CHAR_SELECT)
	_check("disconnected carries its detail",
		String(d3.get("message", "")).find("grant already consumed") != -1)

	# bad_hello — connect_to_world() synchronously refused the WorldHello frame.
	var r4 := Router.new()
	var d4: Dictionary = r4.decide(Router.EVENT_BAD_HELLO)
	_check("pre-handshake bad_hello -> Character Select",
		d4.get("action", "") == Router.ACTION_TO_CHAR_SELECT)
	_check("bad_hello message is non-empty even without detail",
		not String(d4.get("message", "")).is_empty())

	# An unknown/garbage event kind pre-handshake is still treated as a failure.
	var r5 := Router.new()
	var d5: Dictionary = r5.decide("something_unexpected")
	_check("pre-handshake unknown kind -> Character Select (fail-safe)",
		d5.get("action", "") == Router.ACTION_TO_CHAR_SELECT)

	# --- 2. AFTER HandshakeOk, a drop STAYS in the world scene. ---
	var r6 := Router.new()
	r6.note_handshake_ok()
	_check("has_entered_world after HandshakeOk", r6.has_entered_world())
	var d6: Dictionary = r6.decide(Router.EVENT_TRANSPORT_CLOSED, "server went away")
	_check("post-handshake transport_closed -> STAY in world",
		d6.get("action", "") == Router.ACTION_STAY)
	var d7: Dictionary = r6.decide(Router.EVENT_DISCONNECTED, "kicked")
	_check("post-handshake disconnected -> STAY in world",
		d7.get("action", "") == Router.ACTION_STAY)

	# --- 3. No detail -> no dangling parenthesis in the message. ---
	var r8 := Router.new()
	var d8: Dictionary = r8.decide(Router.EVENT_CONNECT_FAILED, "")
	_check("empty detail leaves no empty '()' in the message",
		String(d8.get("message", "")).find("()") == -1)

	print("")
	if _fails == 0:
		print("world-connect router: ALL PASS")
		quit(0)
	else:
		print("world-connect router: %d FAILURE(S)" % _fails)
		quit(1)
