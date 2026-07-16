# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the NPC DIALOG AUTO-CLOSE
# (#851): while a gossip / vendor / trainer window is open, walking out of the NPC's
# interaction range auto-closes it (the same UX as pressing Esc, driven by distance).
# NOT a shipped scene: run it as
#   godot --headless --script res://scenes/world/dialog_autoclose_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the PURE distance decision (world.gd should_autoclose_dialog) closes only when the
#     player is beyond the close threshold, measured HORIZONTALLY (planar — height is
#     ignored, mirroring the server's horizontal_distance gate #842), with a hysteresis
#     band above the 5 m open range so boundary jitter doesn't flicker the window;
#   * the reach constants MIRROR the server gate (5 m open, 5.5 m close);
#   * the event-bus close path (close_gossip / close_vendor / close_trainer) hides each of
#     the three windows and clears the open-window guid the world scene tracks — so the
#     world tick that composes the decision + the close path actually dismisses the view;
#   * an NPC with NO known world position (the dev raw-template-id gossip fallback, or an
#     NPC not in AoI) is NOT auto-closed (distance is unmeasurable — like the server, which
#     only enforces range when the position is knowable).
# Exits 0 on success, 1 on any failed assertion — same shape as hud/econ_verify.gd.
extends SceneTree

const DialogAutoclose := preload("res://scenes/world/dialog_autoclose.gd")
const EventBus := preload("res://hud/event_bus.gd")
const GossipWindow := preload("res://hud/gossip_window.gd")
const VendorWindow := preload("res://hud/vendor_window.gd")
const TrainerWindow := preload("res://hud/trainer_window.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian NPC DIALOG AUTO-CLOSE RUNTIME verify (#851)")

	_verify_reach_constants()
	_verify_decision()
	await _verify_gossip_autoclose()
	await _verify_vendor_autoclose()
	await _verify_trainer_autoclose()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


# --- Reach constants mirror the server gate ----------------------------------

func _verify_reach_constants() -> void:
	print("[reach constants]")
	# The open range mirrors the server gate #842 (kNpcInteractionRangeM == 5 m); the close
	# threshold sits a hair beyond it (hysteresis) so we never flicker at the boundary.
	_check("open range == 5 m (server gate)", is_equal_approx(DialogAutoclose.RANGE_M, 5.0))
	_check("close threshold > open range (hysteresis)",
		DialogAutoclose.CLOSE_M > DialogAutoclose.RANGE_M)
	_check("close threshold == 5.5 m", is_equal_approx(DialogAutoclose.CLOSE_M, 5.5))


# --- Pure distance decision --------------------------------------------------

func _verify_decision() -> void:
	print("[decision]")
	var close_m: float = DialogAutoclose.CLOSE_M   # 5.5 m
	var npc := Vector3(10.0, 0.0, 10.0)

	# Standing on the NPC → stay open.
	_check("player AT the NPC → stay open",
		not DialogAutoclose.should_close(npc, npc, close_m))

	# 3 m away (well inside the open range) → stay open.
	_check("player 3 m away → stay open",
		not DialogAutoclose.should_close(npc + Vector3(3.0, 0.0, 0.0), npc, close_m))

	# 5.2 m away — PAST the 5 m open range but INSIDE the 5.5 m close threshold: the
	# hysteresis band keeps a borderline-jittering player from flickering the window.
	_check("player 5.2 m away (hysteresis band) → stay open",
		not DialogAutoclose.should_close(npc + Vector3(5.2, 0.0, 0.0), npc, close_m))

	# 6 m away (clearly past the close threshold) → close.
	_check("player 6 m away → close",
		DialogAutoclose.should_close(npc + Vector3(6.0, 0.0, 0.0), npc, close_m))

	# Diagonal 6 m in X and 6 m in Z (~8.49 m planar) → close.
	_check("player far diagonally → close",
		DialogAutoclose.should_close(npc + Vector3(6.0, 0.0, 6.0), npc, close_m))

	# HEIGHT ONLY: 100 m straight up (same X/Z) is planar distance 0 → stay open. Proves the
	# decision is horizontal (matching the server's horizontal_distance gate) and won't close
	# when only the vertical offset is large (e.g. standing on a ledge above the NPC).
	_check("player 100 m ABOVE the NPC (same X/Z) → stay open (planar ignores height)",
		not DialogAutoclose.should_close(npc + Vector3(0.0, 100.0, 0.0), npc, close_m))


# --- Integration: each window closes when the tick decides out-of-range ------
# Mirrors world.gd _tick_interaction_autoclose: for each open window, if the NPC's position
# is knowable AND the player is beyond the close threshold, call the bus close path. Feeding
# the NPC's position via the map plays the role of world's _remote_nodes[guid].position; an
# absent guid is an NPC with no known position (dev fallback / out of AoI).
func _simulate_tick(bus, ppos: Vector3, npc_pos: Dictionary) -> void:
	var g: int = bus.gossip_npc()
	if g != 0 and _out_of_reach(g, ppos, npc_pos):
		bus.close_gossip()
	var v: int = bus.vendor_open_npc()
	if v != 0 and _out_of_reach(v, ppos, npc_pos):
		bus.close_vendor()
	var t: int = bus.trainer_open_npc()
	if t != 0 and _out_of_reach(t, ppos, npc_pos):
		bus.close_trainer()


func _out_of_reach(guid: int, ppos: Vector3, npc_pos: Dictionary) -> bool:
	if not npc_pos.has(guid):
		return false  # unmeasurable position → never auto-close (like the server gate)
	return DialogAutoclose.should_close(ppos, npc_pos[guid], DialogAutoclose.CLOSE_M)


func _verify_gossip_autoclose() -> void:
	print("[gossip autoclose]")
	var bus = EventBus.new()
	var win = GossipWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	var npc_guid := 4242
	var npc := Vector3(10.0, 0.0, 10.0)

	# Open the gossip menu on a known NPC.
	bus.publish_gossip_menu(npc_guid, [])
	await _wait(1)
	_check("gossip window visible on open", win.visible)
	_check("bus tracks the open gossip NPC", bus.gossip_npc() == npc_guid)

	# A tick with the player IN range (2 m) leaves it open.
	_simulate_tick(bus, npc + Vector3(2.0, 0.0, 0.0), {npc_guid: npc})
	await _wait(1)
	_check("in-range tick keeps gossip open", win.visible and bus.gossip_npc() == npc_guid)

	# A tick with the player OUT of range (8 m) auto-closes it.
	_simulate_tick(bus, npc + Vector3(8.0, 0.0, 0.0), {npc_guid: npc})
	await _wait(1)
	_check("out-of-range tick closes gossip", not win.visible)
	_check("bus cleared the open gossip NPC", bus.gossip_npc() == 0)

	# Re-open, then a tick where the NPC has NO known position (dev raw-template-id fallback /
	# out of AoI): it must NOT auto-close (distance unmeasurable).
	bus.publish_gossip_menu(npc_guid, [])
	await _wait(1)
	_simulate_tick(bus, npc + Vector3(999.0, 0.0, 0.0), {})  # no position for npc_guid
	await _wait(1)
	_check("unknown-position NPC keeps gossip open", win.visible and bus.gossip_npc() == npc_guid)
	win.queue_free()


func _verify_vendor_autoclose() -> void:
	print("[vendor autoclose]")
	var bus = EventBus.new()
	var win = VendorWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	var npc_guid := 42
	var npc := Vector3(-5.0, 0.0, 20.0)

	bus.open_vendor(npc_guid)
	await _wait(1)
	_check("vendor window visible on open", win.visible)
	_check("bus tracks the open vendor NPC", bus.vendor_open_npc() == npc_guid)

	# In range → stays open.
	_simulate_tick(bus, npc + Vector3(0.0, 0.0, 3.0), {npc_guid: npc})
	await _wait(1)
	_check("in-range tick keeps vendor open", win.visible and bus.vendor_open_npc() == npc_guid)

	# Out of range → auto-closes.
	_simulate_tick(bus, npc + Vector3(0.0, 0.0, 10.0), {npc_guid: npc})
	await _wait(1)
	_check("out-of-range tick closes vendor", not win.visible)
	_check("bus cleared the open vendor NPC", bus.vendor_open_npc() == 0)
	win.queue_free()


func _verify_trainer_autoclose() -> void:
	print("[trainer autoclose]")
	var bus = EventBus.new()
	var win = TrainerWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	var npc_guid := 64
	var npc := Vector3(100.0, 0.0, -40.0)

	# The trainer list is pushed with the gossip menu; opening reveals the window.
	bus.publish_trainer_list(npc_guid, [])
	bus.open_trainer(npc_guid)
	await _wait(1)
	_check("trainer window visible on open", win.visible)
	_check("bus tracks the open trainer NPC", bus.trainer_open_npc() == npc_guid)

	# In range → stays open.
	_simulate_tick(bus, npc + Vector3(4.0, 0.0, 0.0), {npc_guid: npc})
	await _wait(1)
	_check("in-range tick keeps trainer open", win.visible and bus.trainer_open_npc() == npc_guid)

	# Out of range → auto-closes.
	_simulate_tick(bus, npc + Vector3(20.0, 0.0, 0.0), {npc_guid: npc})
	await _wait(1)
	_check("out-of-range tick closes trainer", not win.visible)
	_check("bus cleared the open trainer NPC", bus.trainer_open_npc() == 0)
	win.queue_free()
