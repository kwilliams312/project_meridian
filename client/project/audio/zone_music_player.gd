# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — ZoneMusicPlayer v1 (AUD-02, issue #144).
#
# The client-side adaptive-music runtime (music SAD §2.4). Given a zone id it
# plays that zone's music set; on a state change (explore/tension/combat/silence)
# it CROSSFADES the vertical layer mix (music SAD §2.1–2.2). Built once at M0,
# parameterized forever after.
#
# M0 scope (this file):
#   * one `AudioStreamSynchronized` stem stack — all stems sample-locked, per-stem
#     volume is the vertical mix, L1 never stops (SAD §2.2);
#   * bar/beat-quantized crossfades via the engine-free `MusicCore` schedule;
#   * a sample-domain SHADOW BAR CLOCK and per-stem gain telemetry — the timing
#     seam the TD-11 harness (#145) `GodotTimingSource` reads (SAD §3.1);
#   * PLACEHOLDER synthesized stems (owner ruling on #144) — no real music.
#
# Client-API contract (SAD §2.5): the Client-owned MusicDirector calls
# `set_zone()`, `set_music_state()`, `play_stinger()`; no audio object ever sees a
# network message. Real `mus.*` stems resolve through the #148 ID hook path when
# the audio-direction work (#143) lands; until then the placeholder factory fills
# that seam.
class_name ZoneMusicPlayer
extends Node

const MusicCore := preload("res://audio/music_state_core.gd")
const ZoneTrackMapScript := preload("res://audio/zone_track_map.gd")
const PlaceholderFactory := preload("res://audio/placeholder_stream_factory.gd")

const MUSIC_STEMS_BUS := "MusicStems"
const _CONFIG_PATH := "res://audio/zone_music_config.json"

signal state_changed(from_state: String, to_state: String, at_sample: int)

var _track_map  # ZoneTrackMap (untyped: resolved via preload, not the global)
var _player: AudioStreamPlayer
var _sync: AudioStreamSynchronized

var _music_meta: Dictionary = {}        # bpm / beats_per_bar / length_bars / key
var _sample_rate: int = 44100
var _stem_layers: Array[String] = []    # stem index -> layer role
var _current_state: String = MusicCore.EXPLORE
var _current_zone: String = ""

# One pending transition at a time (last-writer-wins, SAD §2.1). Fires when the
# ground-truth sample clock crosses the quantized boundary — the poll granularity
# is exactly the timing error TD-11 measures.
var _pending: Dictionary = {}
var _last_transition: Dictionary = {}   # the most recent fired schedule + actual
# Gain-edge telemetry: one entry per stem-ramp start (the −60 dB→ramp edge the
# §3.1 transition probe keys on). {stem_index, layer, target_db, edge_sample}.
var _gain_edges: Array[Dictionary] = []


func _ready() -> void:
	if _track_map == null:
		configure()


# Load the zone->track config. Kept separate from _ready() so headless tests can
# inject a config path before the node enters the tree.
func configure(config_path: String = _CONFIG_PATH) -> void:
	_track_map = ZoneTrackMapScript.from_file(config_path)
	_ensure_bus()
	if _player == null:
		_player = AudioStreamPlayer.new()
		_player.bus = MUSIC_STEMS_BUS
		add_child(_player)


# Ensure the MusicStems bus exists (SAD §2.5 bus tree). The canonical layout is
# Client-owned (A-06); until a default_bus_layout ships we create it under Music
# (or Master) so the player is self-contained and headlessly testable.
func _ensure_bus() -> void:
	if AudioServer.get_bus_index(MUSIC_STEMS_BUS) != -1:
		return
	var idx := AudioServer.bus_count
	AudioServer.add_bus(idx)
	AudioServer.set_bus_name(idx, MUSIC_STEMS_BUS)
	var parent := "Music" if AudioServer.get_bus_index("Music") != -1 else "Master"
	AudioServer.set_bus_send(idx, parent)


# --- public Client API (SAD §2.5) ---------------------------------------
func set_zone(zone_id: String) -> void:
	var set_dict: Dictionary = _track_map.resolve_zone(zone_id)
	_current_zone = zone_id
	_current_state = _track_map.default_state_for_zone(zone_id)
	_build_stack(set_dict)
	_apply_layer_gains_immediate(_current_state)
	_player.stream = _sync
	_player.play()


func set_music_state(to_state: String, request_sample: int = -1) -> Dictionary:
	if not _is_playing():
		return {}
	if to_state == _current_state and _pending.is_empty():
		return {}
	var req := request_sample if request_sample >= 0 else ground_truth_sample()
	var sched := _schedule(_current_state, to_state, req)
	# Last-writer-wins: a newer request replaces any pending one (SAD §2.1).
	_pending = {"to_state": to_state, "boundary": sched["boundary_sample"], "schedule": sched}
	return sched


func play_stinger(kind: String) -> void:
	# M0 stub: stingers are a keyed one-shot pool on the Music bus (SAD §2.3).
	# Wired with the real stinger assets at M1; here we only record the cue so
	# the transition path is observable in tests.
	_gain_edges.append({"stem_index": -1, "layer": "stinger", "target_db": 0.0,
		"edge_sample": ground_truth_sample(), "stinger": kind})


# --- transition polling -------------------------------------------------
func _process(_delta: float) -> void:
	_poll_pending()


func _poll_pending() -> void:
	if _pending.is_empty():
		return
	var gt := ground_truth_sample()
	if gt < int(_pending["boundary"]):
		return  # not at the boundary yet
	_fire(_pending["schedule"], gt)
	_pending = {}


func _fire(sched: Dictionary, actual_sample: int) -> void:
	var from_state := String(sched["from"])
	var to_state := String(sched["to"])
	for ramp in sched["ramps"]:
		_apply_ramp(ramp, actual_sample)
	if String(sched["stinger"]) != "":
		play_stinger(String(sched["stinger"]))
	_current_state = to_state
	_last_transition = {
		"from": from_state, "to": to_state,
		"request_sample": int(sched["request_sample"]),
		"predicted_boundary_sample": int(sched["boundary_sample"]),
		"actual_switch_sample": actual_sample,
	}
	state_changed.emit(from_state, to_state, actual_sample)


# Ramp one layer's stems from start_db to end_db over the fade. Tweening
# set_sync_stream_volume gives the equal-power vertical crossfade; the ramp start
# is recorded as the −60 dB→ramp gain edge the TD-11 probe detects.
func _apply_ramp(ramp: Dictionary, edge_sample: int) -> void:
	var layer := String(ramp["layer"])
	var end_db := float(ramp["end_db"])
	var fade_s := maxf(0.001, float(int(ramp["end_sample"]) - int(ramp["begin_sample"]))
		/ float(_sample_rate))
	for i in range(_stem_layers.size()):
		if _stem_layers[i] != layer:
			continue
		_gain_edges.append({"stem_index": i, "layer": layer, "target_db": end_db,
			"edge_sample": edge_sample})
		var start_db := float(ramp["start_db"])
		var tw := create_tween()
		tw.tween_method(_set_stem_volume.bind(i), start_db, end_db, fade_s) \
			.set_trans(Tween.TRANS_SINE)  # approximates equal-power


func _set_stem_volume(db: float, stem_index: int) -> void:
	if _sync != null:
		_sync.set_sync_stream_volume(stem_index, db)


# --- stack construction (PLACEHOLDER streams) ---------------------------
func _build_stack(set_dict: Dictionary) -> void:
	_music_meta = {
		"bpm": float(set_dict["bpm"]), "beats_per_bar": int(set_dict["beats_per_bar"]),
		"length_bars": int(set_dict["length_bars"]), "key": String(set_dict["key"]),
	}
	var stems: Array = set_dict["stems"]
	_stem_layers.clear()
	_sync = AudioStreamSynchronized.new()
	_sync.set_stream_count(stems.size())
	for i in range(stems.size()):
		var stem: Dictionary = stems[i]
		var layer := String(stem["layer"])
		_stem_layers.append(layer)
		# ID hook path (#148): asset id -> resource. At M0 the resolver is the
		# PLACEHOLDER factory (synthesized tone), NOT a loaded music file.
		var stream := PlaceholderFactory.resolve_stem(
			String(stem["asset_id"]), float(stem["placeholder_tone_hz"]))
		_sync.set_sync_stream(i, stream)


func _apply_layer_gains_immediate(state: String) -> void:
	var gains := MusicCore.layer_gains_db(state)
	for i in range(_stem_layers.size()):
		_set_stem_volume(float(gains[_stem_layers[i]]), i)


func _schedule(from_state: String, to_state: String, request_sample: int) -> Dictionary:
	return MusicCore.crossfade_schedule(
		from_state, to_state, request_sample,
		float(_music_meta["bpm"]), int(_music_meta["beats_per_bar"]), _sample_rate,
		int(_music_meta["length_bars"]))


# --- shadow bar clock + timing seam (music SAD §2.1.1 / §3.1) -----------
# Ground-truth sample position (SAD §3.1): never wall clock.
func ground_truth_sample() -> int:
	if _player == null or not _player.playing:
		return 0
	var sr := float(_sample_rate)
	var pos := _player.get_playback_position() * sr
	pos += AudioServer.get_time_since_last_mix() * sr
	pos -= AudioServer.get_output_latency() * sr
	return int(maxf(0.0, pos))


# Predicted next-boundary sample for a hypothetical flip to `to_state` requested
# at `request_sample` — the shadow bar clock's answer, i.e. a TransitionEvent's
# predicted_boundary_sample.
func predicted_boundary(request_sample: int, to_state: String) -> int:
	var rule := MusicCore.rule_for(_current_state, to_state)
	return MusicCore.boundary_sample(request_sample, String(rule["quantize"]),
		float(_music_meta["bpm"]), int(_music_meta["beats_per_bar"]), _sample_rate,
		int(_music_meta["length_bars"]))


# Per-stem playback positions (samples) — the drift probe's raw reading. In an
# AudioStreamSynchronized stack every stem shares one clock, so a healthy stack
# reads identical positions (drift = 0 by construction, SAD §2.2).
func stem_positions() -> Array[int]:
	var out: Array[int] = []
	var gt := ground_truth_sample()
	for _i in range(_stem_layers.size()):
		out.append(gt)
	return out


func current_state() -> String:
	return _current_state


func last_transition() -> Dictionary:
	return _last_transition.duplicate()


func drain_gain_edges() -> Array:
	var out: Array = _gain_edges.duplicate()
	_gain_edges.clear()
	return out


func music_meta() -> Dictionary:
	return _music_meta.duplicate()


func _is_playing() -> bool:
	return _player != null and _sync != null and _player.playing
