# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — world-connect ROUTING DECISION (issue #301 UX).
#
# A tiny, PURE state machine that decides what should happen when the networked
# world scene's connection to worldd ends or fails. It exists so the routing
# decision is UNIT-TESTABLE HEADLESSLY: it has no Godot nodes, no GDExtension, no
# sockets, no SceneTree — just (phase + event) -> decision. scenes/world/world.gd
# feeds it the real connection signals and acts on the returned action; the test
# (scenes/world/world_connect_router_verify.gd) drives it with synthetic events.
#
# THE RULE (the owner's UX ask): if the connection fails BEFORE HandshakeOk, the
# player never actually entered the world — do NOT strand them in an empty scene.
# Route them back to Character Select with the error surfaced. Once HandshakeOk has
# arrived (they ARE in the world), a later drop is an in-world disconnect: stay in
# the world scene and report it on the HUD (a reconnect story owns that path).

extends RefCounted
class_name MeridianWorldConnectRouter

## What world.gd should do with a connection-ended/failed event.
const ACTION_STAY := "stay"                       ## remain in the world scene
const ACTION_TO_CHAR_SELECT := "to_char_select"   ## bounce back to Character Select

## Event kinds world.gd forwards (mirror MeridianNetThread's signals + the
## synchronous connect_to_world() == false case).
const EVENT_CONNECT_FAILED := "connect_failed"    ## never reached worldd (TLS/socket)
const EVENT_TRANSPORT_CLOSED := "transport_closed" ## peer closed the transport
const EVENT_DISCONNECTED := "disconnected"        ## worldd sent a Disconnect
const EVENT_BAD_HELLO := "bad_hello"              ## connect_to_world() refused the frame

# Set once HandshakeOk arrives. Before that we are PRE-handshake (still connecting).
var _handshake_seen := false


## Record that the world session reached HandshakeOk (the player is now IN world).
func note_handshake_ok() -> void:
	_handshake_seen = true


## True once HandshakeOk has been observed.
func has_entered_world() -> bool:
	return _handshake_seen


## Decide what to do for a connection-ended/failed event.
##   kind:   one of the EVENT_* constants (unknown kinds are treated as a failure)
##   detail: human-readable reason from the transport/server (may be empty)
## Returns { "action": ACTION_*, "message": String }. `message` is the player-facing
## line: a Character-Select error before handshake, or an in-world HUD note after.
func decide(kind: String, detail: String = "") -> Dictionary:
	if _handshake_seen:
		# Already in the world: this is an in-world disconnect, not a failure to
		# enter. Stay put and let the HUD/report show what happened.
		return {"action": ACTION_STAY, "message": _in_world_message(kind, detail)}
	# Pre-HandshakeOk: entering the world failed. Send the player back to Character
	# Select with a clear reason instead of leaving them in an empty world.
	return {"action": ACTION_TO_CHAR_SELECT, "message": _fail_message(kind, detail)}


# --- Message helpers (plain strings; a real UI can localize by kind later) ----

func _fail_message(kind: String, detail: String) -> String:
	var base := ""
	match kind:
		EVENT_CONNECT_FAILED:
			base = "Could not reach the world server"
		EVENT_TRANSPORT_CLOSED:
			base = "The world server closed the connection before you entered"
		EVENT_DISCONNECTED:
			base = "The world server refused entry"
		EVENT_BAD_HELLO:
			base = "Could not start the world session (invalid session handoff)"
		_:
			base = "Could not enter the world"
	return "%s.%s Returned to Character Select — try Enter World again." \
		% [base, _detail_suffix(detail)]


func _in_world_message(kind: String, detail: String) -> String:
	var base := ""
	match kind:
		EVENT_TRANSPORT_CLOSED:
			base = "Connection to the world closed"
		EVENT_DISCONNECTED:
			base = "Disconnected from the world"
		_:
			base = "Lost the world connection"
	return "%s.%s" % [base, _detail_suffix(detail)]


func _detail_suffix(detail: String) -> String:
	var trimmed := detail.strip_edges()
	return "" if trimmed.is_empty() else " (%s)" % trimmed
