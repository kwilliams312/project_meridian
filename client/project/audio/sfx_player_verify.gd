# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for SfxPlayer (#148).
# NOT a shipped scene. Run:
#   godot --headless --path client/project --script res://audio/sfx_player_verify.gd
# Proves, with NO display and NO audio device (Dummy driver), that:
#   * the engine-free SfxEventMap matches the golden values the Python reference
#     (tools/zone_sfx, tests/test_zone_sfx.py) asserts;
#   * SfxPlayer instantiates, builds the SFX/UI bus tree, resolves an event ->
#     sfx id -> PLACEHOLDER one-shot via the ID hook path, and routes it to the
#     correct bus;
#   * a content-carried id (ability impact_sfx) resolves through the SAME hook.
# The actual AUDIO PLAYBACK / one-shot *sound* needs a device — owner confirms.
# Exits 0 on success, 1 on any failed assertion.
extends SceneTree

const SfxEventMapScript := preload("res://audio/sfx_event_map.gd")
const SfxPlayerScript := preload("res://audio/sfx_player.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("SfxPlayer RUNTIME verify (#148) — PLACEHOLDER audio")

	# --- 1. Engine-free event map: golden values (mirror test_zone_sfx.py). ---
	var m = SfxEventMapScript.from_file("res://audio/sfx_config.json")
	_check("placeholder config", m.placeholder() == true)
	_check("~10 placeholder one-shots", m.sfx_ids().size() >= 10)
	_check("ui.click event -> id", m.resolve_event("ui.click") == "core:sfx.ui.click")
	_check("login.sting event -> id",
		m.resolve_event("login.sting") == "core:sfx.ui.login_sting")
	_check("footstep by surface -> id",
		m.resolve_event(m.footstep_event("grass")) == "core:sfx.foley.footstep.grass")
	_check("ui routes to UI bus", m.bus_for("core:sfx.ui.click") == "UI")
	_check("footstep routes to SFX_Foley",
		m.bus_for("core:sfx.foley.footstep.grass") == "SFX_Foley")
	_check("combat routes to SFX_Combat",
		m.bus_for("core:sfx.combat.impact.generic") == "SFX_Combat")
	_check("content-carried id is a valid sfx id",
		m.is_valid_sfx_id("core:sfx.combat.impact.pickaxe"))
	_check("art id is NOT a valid sfx id",
		not m.is_valid_sfx_id("core:art.vfx.impact.dust_small"))

	# --- 2. SfxPlayer instantiation + bus tree. ---
	var sp = SfxPlayerScript.new()
	root.add_child(sp)
	sp.configure()  # loads res://audio/sfx_config.json
	await _wait(2)  # real frames — Dummy audio driver advances the mix

	_check("player is a Node", sp is Node)
	_check("SFX bus exists", AudioServer.get_bus_index("SFX") != -1)
	_check("UI bus exists", AudioServer.get_bus_index("UI") != -1)
	_check("SFX_Foley child bus exists", AudioServer.get_bus_index("SFX_Foley") != -1)

	# --- 3. Trigger an event through the ID hook path. ---
	var sfx_id := String(sp.play_event("ui.click"))
	await _wait(1)
	_check("play_event resolved ui.click", sfx_id == "core:sfx.ui.click")
	var lt: Dictionary = sp.last_trigger()
	_check("triggered on the UI bus", String(lt.get("bus", "")) == "UI")
	_check("resource is tagged PLACEHOLDER",
		String(lt.get("resource_name", "")).begins_with("PLACEHOLDER:"))

	# --- 4. Footstep + content-carried id through the SAME hook. ---
	sp.play_footstep("stone")
	await _wait(1)
	_check("footstep routed to SFX_Foley",
		String(sp.last_trigger().get("bus", "")) == "SFX_Foley")

	sp.play_content_sfx("core:sfx.combat.impact.pickaxe")
	await _wait(1)
	var ct: Dictionary = sp.last_trigger()
	_check("content id resolved through the hook",
		String(ct.get("sfx_id", "")) == "core:sfx.combat.impact.pickaxe")

	sp.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("SfxPlayer verify: ALL PASS")
		quit(0)
	else:
		print("SfxPlayer verify: %d FAILURE(S)" % _fails)
		quit(1)
