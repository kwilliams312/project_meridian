# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for FLOATING COMBAT TEXT (CMB-04,
# #530; combat-presentation epic #23). NOT a shipped scene: run under
#   godot --headless --script res://scenes/world/combat_text_verify.gd
# so CI / a dev box can prove, with REAL frames and NO display and NO server, that:
#   * the world scene builds the POOLED floating-text system and preallocates its labels
#     (no per-hit allocation — the free pool is full at rest, nothing active);
#   * a CAST_RESULT published on the event bus (cast_result_received — the SAME seam the
#     net path drives) floats a number over the TARGET entity's on-screen node, using the
#     guid→Node3D map (#496): a remote target and the LOCAL player both anchor correctly;
#   * the number is styled straight from the server frame — damage vs heal vs a crit
#     (bigger) vs an avoided hit (Miss/Dodge/Parry) — the client inventing nothing;
#   * a CAST_RESULT whose target is not on screen floats nothing (no anchor);
#   * a spawned number RECYCLES back into the pool after its lifetime (the pool is whole
#     again — proof of reuse, not a leak);
#   * the pool never grows past POOL_SIZE under a burst (the oldest recycles early).
# It drives world.tscn in its OFFLINE (no-session) path — no socket is ever opened.
# Exits 0 on success, 1 on any failed assertion — same shape as target_verify.gd.

extends SceneTree

const FloatingCombatText := preload("res://scenes/world/floating_combat_text.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("meridian FLOATING COMBAT TEXT runtime verify (#530)")

	var packed := load("res://scenes/world/world.tscn") as PackedScene
	_check("world.tscn loads", packed != null)
	if packed == null:
		quit(1)
		return
	var world = packed.instantiate()
	# OFFLINE (no session) — _ready() builds the player/camera/HUD/event bus + the pooled
	# floating-text node, never a socket.
	world.configure({}, {"name": "Verifier", "level": 1, "class": 1})
	root.add_child(world)
	await _wait(5)  # real frames — the scene builds and the pool preallocates

	var bus = world._bus
	var ft = world._floating_text
	_check("event bus built", bus != null)
	_check("floating-text system built", ft != null)
	if ft == null:
		world.queue_free()
		quit(1)
		return

	# Freeze auto-advance so the animation clock is driven ONLY by explicit _advance()
	# steps below — a number's style/position is then exact right after spawn (no partial
	# rise/fade from an intervening process frame), and recycling is deterministic.
	ft.set_process(false)

	# --- Pool is preallocated and idle at rest (NO per-hit allocation). ----------------
	_check("pool preallocated to POOL_SIZE labels", ft.free_count() == ft.pool_size())
	_check("nothing active at rest", ft.active_count() == 0)
	# The pooled labels exist as hidden children up front (recycled, never new()'d per hit).
	var label_children := 0
	for child in ft.get_children():
		if child is Label3D:
			label_children += 1
	_check("all pool labels exist as child nodes", label_children == ft.pool_size())

	# --- Spawn a remote target and give it vitals (so the guid→Node3D map is populated). ---
	var mob_guid := 0x64
	var mob_pos := Vector3(64.0, 0.0, 72.0)
	world._spawn_remote(mob_guid, {"position": mob_pos, "char_class": 2})
	await _wait(2)
	_check("remote target node spawned", world._remote_nodes.has(mob_guid))
	var mob_node = world._remote_nodes[mob_guid]

	# --- A damage CAST_RESULT floats a number over the target, via the bus seam. --------
	bus.publish_cast_result({
		"ability_id": 301, "caster_guid": 0x1, "target_guid": mob_guid,
		"outcome": FloatingCombatText.OUTCOME_HIT, "amount": 123, "is_heal": false,
		"target_health": 200, "target_dead": false, "server_time_ms": 1000,
	}, 1000)
	await _wait(1)
	_check("damage CAST_RESULT spawned one number", ft.active_count() == 1)
	var dmg = ft.newest_active_label()
	_check("damage number shows the server amount", dmg != null and dmg.text == "123")
	_check("damage number is visible", dmg != null and dmg.visible)
	_check("damage number is billboarded (faces camera)",
		dmg != null and dmg.billboard == BaseMaterial3D.BILLBOARD_ENABLED)
	# Positioned over the TARGET node: same x/z, raised by HEAD_OFFSET above its origin.
	_check("number sits over the target node (x/z match)",
		dmg != null and absf(dmg.global_position.x - mob_node.global_position.x) < 0.01
		and absf(dmg.global_position.z - mob_node.global_position.z) < 0.01)
	_check("number sits above the target head",
		dmg != null and absf(dmg.global_position.y
			- (mob_node.global_position.y + FloatingCombatText.HEAD_OFFSET)) < 0.01)
	_check("damage number uses the damage color",
		dmg != null and dmg.modulate.is_equal_approx(FloatingCombatText.COL_DAMAGE))
	_check("damage number uses the base font size",
		dmg != null and dmg.font_size == FloatingCombatText.FONT_SIZE_BASE)

	# --- Recycle after lifetime: the number returns to the pool (reuse, not a leak). ----
	ft._advance(FloatingCombatText.LIFETIME + 0.2)
	_check("number recycled after its lifetime", ft.active_count() == 0)
	_check("pool whole again after recycle", ft.free_count() == ft.pool_size())
	_check("recycled label hidden", dmg != null and not dmg.visible)

	# --- A CRIT reads BIGGER and hotter than a normal hit. ------------------------------
	bus.publish_cast_result({
		"target_guid": mob_guid, "outcome": FloatingCombatText.OUTCOME_CRIT,
		"amount": 240, "is_heal": false,
	}, 1100)
	var crit = ft.newest_active_label()
	_check("crit number shows its amount", crit != null and crit.text == "240")
	_check("crit number is larger than a normal hit",
		crit != null and crit.font_size == FloatingCombatText.FONT_SIZE_CRIT
		and FloatingCombatText.FONT_SIZE_CRIT > FloatingCombatText.FONT_SIZE_BASE)
	_check("crit number uses the crit color",
		crit != null and crit.modulate.is_equal_approx(FloatingCombatText.COL_CRIT))
	ft._advance(FloatingCombatText.LIFETIME + 0.2)  # clear

	# --- A HEAL is green and prefixed with '+'. -----------------------------------------
	bus.publish_cast_result({
		"target_guid": mob_guid, "outcome": FloatingCombatText.OUTCOME_HIT,
		"amount": 88, "is_heal": true,
	}, 1200)
	var heal = ft.newest_active_label()
	_check("heal number is prefixed with '+'", heal != null and heal.text == "+88")
	_check("heal number uses the heal color",
		heal != null and heal.modulate.is_equal_approx(FloatingCombatText.COL_HEAL))
	ft._advance(FloatingCombatText.LIFETIME + 0.2)  # clear

	# --- An AVOIDED hit shows the outcome word, not a number. ---------------------------
	bus.publish_cast_result({
		"target_guid": mob_guid, "outcome": FloatingCombatText.OUTCOME_DODGE,
		"amount": 0, "is_heal": false,
	}, 1300)
	var dodge = ft.newest_active_label()
	_check("dodge shows the outcome word", dodge != null and dodge.text == "Dodge")
	_check("avoided hit uses the muted color",
		dodge != null and dodge.modulate.is_equal_approx(FloatingCombatText.COL_AVOID))
	ft._advance(FloatingCombatText.LIFETIME + 0.2)  # clear

	# --- The LOCAL player is a valid anchor too (a mob hits YOU). -----------------------
	world._my_guid = 0x9001
	bus.publish_cast_result({
		"target_guid": 0x9001, "outcome": FloatingCombatText.OUTCOME_HIT,
		"amount": 45, "is_heal": false,
	}, 1400)
	var self_hit = ft.newest_active_label()
	_check("a hit on the local player floats over the player node",
		self_hit != null and ft.active_count() == 1
		and absf(self_hit.global_position.x - world._player.global_position.x) < 0.01
		and absf(self_hit.global_position.z - world._player.global_position.z) < 0.01)
	ft._advance(FloatingCombatText.LIFETIME + 0.2)  # clear

	# --- A target that is NOT on screen anchors nothing (no node → no number). ----------
	bus.publish_cast_result({
		"target_guid": 0xDEAD, "outcome": FloatingCombatText.OUTCOME_HIT,
		"amount": 500, "is_heal": false,
	}, 1500)
	_check("off-screen target floats nothing", ft.active_count() == 0)

	# --- Under a burst the pool never grows past POOL_SIZE (oldest recycles early). -----
	var burst: int = ft.pool_size() + 6
	for n in range(burst):
		bus.publish_cast_result({
			"target_guid": mob_guid, "outcome": FloatingCombatText.OUTCOME_HIT,
			"amount": n, "is_heal": false,
		}, 1600 + n)
	_check("burst never exceeds the pool size", ft.active_count() == ft.pool_size())
	_check("burst allocated no extra label nodes", _count_label3d(ft) == ft.pool_size())
	ft._advance(FloatingCombatText.LIFETIME + 0.2)  # clear
	_check("pool whole again after the burst", ft.free_count() == ft.pool_size())

	world.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("combat text verify: ALL PASS")
		quit(0)
	else:
		print("combat text verify: %d FAILURE(S)" % _fails)
		quit(1)


func _count_label3d(node: Node) -> int:
	var count := 0
	for child in node.get_children():
		if child is Label3D:
			count += 1
	return count
