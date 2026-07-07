# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for ZoneMusicPlayer (#144).
# NOT a shipped scene. Run:
#   godot --headless --path client/project --script res://audio/zone_music_player_verify.gd
# Proves, with NO display and NO audio device (Dummy driver), that:
#   * the engine-free MusicCore + ZoneTrackMap match the golden values the
#     Python reference (tools/zone_music, tests/test_zone_music.py) asserts;
#   * ZoneMusicPlayer instantiates, loads the PLACEHOLDER stem stack via the ID
#     hook path, applies the explore vertical mix, and schedules a bar/beat-
#     quantized crossfade with the right rising/falling layer ramps + stinger;
#   * the timing seam (predicted boundary, per-stem positions) reads correctly.
# The actual AUDIO PLAYBACK / crossfade *sound* needs a device — owner confirms.
# Exits 0 on success, 1 on any failed assertion.
extends SceneTree

const MusicCore := preload("res://audio/music_state_core.gd")
const ZoneMusicPlayerScript := preload("res://audio/zone_music_player.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	print("ZoneMusicPlayer RUNTIME verify (#144) — PLACEHOLDER audio")

	# --- 1. Engine-free core: golden values (mirror tests/test_zone_music.py). ---
	_check("director explore when idle",
		MusicCore.director_state(false, false) == MusicCore.EXPLORE)
	_check("director tension on hostile",
		MusicCore.director_state(false, true) == MusicCore.TENSION)
	_check("director combat flag beats tension",
		MusicCore.director_state(true, true) == MusicCore.COMBAT)

	var eg := MusicCore.layer_gains_db(MusicCore.EXPLORE)
	_check("explore = L1+L2 only",
		eg["L1"] == 0.0 and eg["L2"] == 0.0 and eg["L3"] == -60.0 and eg["L4"] == -60.0)
	var cg := MusicCore.layer_gains_db(MusicCore.COMBAT)
	_check("combat = L1+L3+L4, L2 ducked",
		cg["L1"] == 0.0 and cg["L3"] == 0.0 and cg["L4"] == 0.0 and cg["L2"] == -60.0)

	var enter := MusicCore.rule_for(MusicCore.EXPLORE, MusicCore.COMBAT)
	_check("combat entry = beat + 500ms + stinger",
		enter["quantize"] == "beat" and enter["fade_ms"] == 500.0
		and enter["stinger"] == "combat_enter")
	var exit := MusicCore.rule_for(MusicCore.COMBAT, MusicCore.EXPLORE)
	_check("combat exit = 4s hysteresis, 2-bar bar-fade",
		exit["quantize"] == "bar" and exit["hysteresis_s"] == 4.0 and exit["fade_bars"] == 2.0)

	# Bar clock golden: 96 BPM 4/4 @ 44.1k -> 110250 samples/bar (SAD §4.2).
	var spb := MusicCore.samples_per_bar(96.0, 4, 44100)
	_check("110250 samples/bar", int(round(spb)) == 110250)
	_check("next-bar boundary at/after request",
		MusicCore.boundary_sample(1, "bar", 96.0, 4, 44100) == 110250)
	# 44100*60/96 = 27562.5 samples/beat; first beat at/after sample 1 (allow ±1
	# sample for the half-value rounding difference vs the Python reference).
	var beat_b := MusicCore.boundary_sample(1, "beat", 96.0, 4, 44100)
	_check("beat boundary for combat entry", beat_b == 27562 or beat_b == 27563)

	# --- 2. ZoneMusicPlayer instantiation + placeholder stack. ---
	var zmp = ZoneMusicPlayerScript.new()
	root.add_child(zmp)
	zmp.configure()  # loads res://audio/zone_music_config.json
	zmp.set_zone("zone.bootstrap")
	await _wait(3)  # real frames — Dummy audio driver advances the mix

	_check("player is a Node", zmp is Node)
	_check("MusicStems bus exists", AudioServer.get_bus_index("MusicStems") != -1)
	_check("starts in explore (zone default)", zmp.current_state() == "explore")
	var meta: Dictionary = zmp.set_meta()
	_check("set meta bpm 96 / 4-4 / 96 bars",
		meta["bpm"] == 96.0 and meta["beats_per_bar"] == 4 and meta["length_bars"] == 96)
	var positions = zmp.stem_positions()
	_check("5-stem synchronized stack", positions.size() == 5)

	# --- 3. Schedule a combat flip: beat-quantized, L3/L4 rise, stinger. ---
	var sched: Dictionary = zmp.set_music_state("combat", 5000)
	_check("combat schedule beat-quantized", String(sched.get("quantize", "")) == "beat")
	_check("combat schedule fires stinger", String(sched.get("stinger", "")) == "combat_enter")
	var rising := {}
	for r in sched.get("ramps", []):
		rising[String(r["layer"])] = float(r["end_db"]) > float(r["start_db"])
	_check("L3 rises into combat", rising.get("L3", false) == true)
	_check("L4 rises into combat", rising.get("L4", false) == true)
	_check("L2 ducks under combat", rising.get("L2", true) == false)
	_check("boundary >= request", int(sched["boundary_sample"]) >= 5000)

	# --- 4. Timing seam reads. ---
	_check("predicted_boundary matches schedule",
		zmp.predicted_boundary(5000, "combat") == int(sched["boundary_sample"]))

	zmp.queue_free()
	await _wait(1)

	print("")
	if _fails == 0:
		print("ZoneMusicPlayer verify: ALL PASS")
		quit(0)
	else:
		print("ZoneMusicPlayer verify: %d FAILURE(S)" % _fails)
		quit(1)
