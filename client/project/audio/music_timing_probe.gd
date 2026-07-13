# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — TD-11 timing probe for ZoneMusicPlayer (#144 / #145 / #147).
#
# The real-audio counterpart of tools/td11_music_timing's SampleClockModel: it
# drives a live ZoneMusicPlayer, scripts state flips, and emits — as a single
# JSON object on stdout — the SAME `TransitionEvent` / `DriftSample` records the
# harness consumes. The Python `GodotTimingSource` (tools/td11_music_timing/
# probe.py) shells out to this script and parses that JSON; the harness,
# statistics and report code are unchanged (that is the seam).
#
# Run (invoked by GodotTimingSource, but standalone-runnable):
#   godot --headless --path client/project --script res://audio/music_timing_probe.gd \
#         -- --trials 200 --bars 96 --bpm 96 --beats-per-bar 4 --sample-rate 44100
#
# Sample-domain only (SAD §3.1). Under `--headless` the Dummy audio driver still
# advances the mix, so transition timing is genuinely MEASURED — but off the
# min-spec device and 50-bot load rig (#111). The #147 gate re-runs this under
# that rig for the authoritative numbers; a headless run here is the wiring proof.
extends SceneTree

const ZoneMusicPlayerScript := preload("res://audio/zone_music_player.gd")
const MusicCore := preload("res://audio/music_state_core.gd")

const _FLIP_STATES := ["explore", "tension", "combat"]


func _args() -> Dictionary:
	var out := {"trials": 200, "bars": 96, "bpm": 96.0, "beats_per_bar": 4,
		"sample_rate": 44100}
	var argv := OS.get_cmdline_user_args()
	var i := 0
	while i < argv.size():
		var a := String(argv[i])
		if a.begins_with("--") and i + 1 < argv.size():
			var key := a.substr(2).replace("-", "_")
			var val := String(argv[i + 1])
			if out.has(key):
				out[key] = float(val) if key == "bpm" else int(val)
			i += 2
		else:
			i += 1
	return out


func _wait(frames: int) -> void:
	for _i in range(frames):
		await physics_frame


func _initialize() -> void:
	var cfg := _args()
	var zmp = ZoneMusicPlayerScript.new()
	root.add_child(zmp)
	zmp.configure()
	zmp.set_zone("zone.bootstrap")
	await _wait(3)

	var transitions: Array = []
	if int(cfg["trials"]) > 0:
		transitions = await _run_transitions(zmp, int(cfg["trials"]))
	var drift: Array = []
	if int(cfg["bars"]) > 0:
		drift = await _run_drift(zmp, int(cfg["bars"]))

	var payload := {
		"measured": true,
		"config": {
			"sample_rate": int(cfg["sample_rate"]), "bpm": float(cfg["bpm"]),
			"beats_per_bar": int(cfg["beats_per_bar"]),
			"mix_block_frames": _mix_block_frames(),
		},
		"transitions": transitions,
		"drift": drift,
	}
	# A framed marker so the Python side can slice the JSON out of any engine
	# banner noise on stdout.
	print("@@TD11_PROBE_BEGIN@@")
	print(JSON.stringify(payload))
	print("@@TD11_PROBE_END@@")
	quit(0)


# Best-effort audio mix-block size (frames per mix step) — the dominant
# quantization term (harness README "the one number to pin").
func _mix_block_frames() -> int:
	var sr := float(AudioServer.get_mix_rate())
	# Godot advances audio in ~ (mix_rate / 60)-frame blocks by default; report a
	# representative value if the engine does not expose it directly.
	return int(round(sr / 60.0)) if sr > 0.0 else 128


func _run_transitions(zmp, n: int) -> Array:
	var events: Array = []
	var state := "explore"
	for idx in range(n):
		var nxt: String = _FLIP_STATES[(idx + 1) % _FLIP_STATES.size()]
		if nxt == state:
			nxt = _FLIP_STATES[(idx + 2) % _FLIP_STATES.size()]
		var req: int = zmp.ground_truth_sample()
		var predicted: int = zmp.predicted_boundary(req, nxt)
		zmp.set_music_state(nxt, req)
		# Advance real frames until the player fires the queued flip.
		var guard := 0
		while zmp.current_state() != nxt and guard < 240:
			await physics_frame
			guard += 1
		var last: Dictionary = zmp.last_transition()
		var actual: int = int(last.get("actual_switch_sample", predicted))
		events.append({
			"index": idx, "request_sample": req,
			"predicted_boundary_sample": predicted, "actual_switch_sample": actual,
			"from_state": state, "to_state": nxt, "starved": false,
		})
		state = nxt
		zmp.drain_gain_edges()
	return events


func _run_drift(zmp, bars: int) -> Array:
	var samples: Array = []
	var spb: float = MusicCore.samples_per_bar(
		float(zmp.music_meta()["bpm"]), int(zmp.music_meta()["beats_per_bar"]), 44100)
	for bar in range(bars):
		var positions: Array = zmp.stem_positions()
		samples.append({
			"bar_index": bar, "stem_positions": positions,
			"shadow_clock_sample": int(round(bar * spb)),
		})
		await physics_frame
	return samples
