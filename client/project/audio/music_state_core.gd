# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — ZoneMusicPlayer state machine + crossfade scheduler (#144).
#
# Engine-free core (extends RefCounted, touches no AudioServer/AudioStream): the
# explore/tension/combat/silence state machine, the vertical layer mix, the
# transition table (music SAD §2.1), and the sample-domain crossfade schedule.
#
# This is a 1:1 mirror of the Python reference `tools/zone_music/state_core.py`
# (the two are pinned to the same golden values by tests). Keeping the logic here
# — not buried in the audio node — is what makes it headlessly testable and what
# lets the GDExtension fallback (SAD §3.2) reuse it unchanged.
class_name MusicStateCore
extends RefCounted

const EXPLORE := "explore"
const TENSION := "tension"
const COMBAT := "combat"
const SILENCE := "silence"

const LAYERS := ["L1", "L2", "L3", "L4"]

const FULL_DB := 0.0
const FLOOR_DB := -60.0  # inaudible-but-not-stopped floor (music SAD §2.2)

# Quantization kinds (music SAD §2.1).
const Q_BAR := "bar"
const Q_BEAT := "beat"
const Q_LOOP_END := "loop_end"
const Q_IMMEDIATE := "immediate"

# Vertical mix per state: layer -> target gain dB. L1 is always audible.
# Explicit string keys (== the EXPLORE/TENSION/... const values) so the mapping
# is unambiguous regardless of GDScript's bare-identifier dict-key handling.
const STATE_LAYER_GAINS := {
	"explore": {"L1": FULL_DB, "L2": FULL_DB, "L3": FLOOR_DB, "L4": FLOOR_DB},
	"tension": {"L1": FULL_DB, "L2": FLOOR_DB, "L3": FULL_DB, "L4": FLOOR_DB},
	"combat": {"L1": FULL_DB, "L2": FLOOR_DB, "L3": FULL_DB, "L4": FULL_DB},
	"silence": {"L1": FLOOR_DB, "L2": FLOOR_DB, "L3": FLOOR_DB, "L4": FLOOR_DB},
}


# --- state selection (the MusicDirector reduction, SAD §2.4) -------------
static func director_state(combat_flag: bool, hostile_proximity: bool,
		boss_encounter: bool = false) -> String:
	if combat_flag or boss_encounter:
		return COMBAT
	if hostile_proximity:
		return TENSION
	return EXPLORE


static func layer_gains_db(state: String) -> Dictionary:
	assert(STATE_LAYER_GAINS.has(state), "unknown music state: %s" % state)
	return (STATE_LAYER_GAINS[state] as Dictionary).duplicate()


# --- transition table (music SAD §2.1) ----------------------------------
# Each row: {quantize, hysteresis_s, fade_bars, fade_ms, stinger}.
static func rule_for(from_state: String, to_state: String) -> Dictionary:
	var table := {
		[EXPLORE, TENSION]: _rule(Q_BAR, 0.0, 1.0, 0.0, ""),
		[EXPLORE, COMBAT]: _rule(Q_BEAT, 0.0, 0.0, 500.0, "combat_enter"),
		[TENSION, COMBAT]: _rule(Q_BEAT, 0.0, 0.0, 500.0, "combat_enter"),
		[COMBAT, TENSION]: _rule(Q_BAR, 4.0, 2.0, 0.0, "combat_end"),
		[COMBAT, EXPLORE]: _rule(Q_BAR, 4.0, 2.0, 0.0, "combat_end"),
		[TENSION, EXPLORE]: _rule(Q_BAR, 6.0, 2.0, 0.0, ""),
		[EXPLORE, SILENCE]: _rule(Q_LOOP_END, 0.0, 2.0, 0.0, ""),
		[SILENCE, EXPLORE]: _rule(Q_IMMEDIATE, 0.0, 1.0, 0.0, ""),
		[SILENCE, COMBAT]: _rule(Q_IMMEDIATE, 0.0, 0.0, 500.0, "combat_enter"),
		[SILENCE, TENSION]: _rule(Q_IMMEDIATE, 0.0, 0.0, 500.0, ""),
	}
	var key := [from_state, to_state]
	if table.has(key):
		return table[key]
	# §2.1 "any -> any set change" default: next bar, 2-bar equal-power fade.
	return _rule(Q_BAR, 0.0, 2.0, 0.0, "")


static func _rule(quantize: String, hysteresis_s: float, fade_bars: float,
		fade_ms: float, stinger: String) -> Dictionary:
	return {
		"quantize": quantize, "hysteresis_s": hysteresis_s,
		"fade_bars": fade_bars, "fade_ms": fade_ms, "stinger": stinger,
	}


# --- sample-domain bar clock (music SAD §2.1.1) -------------------------
static func samples_per_beat(bpm: float, sample_rate: int) -> float:
	return float(sample_rate) * 60.0 / bpm


static func samples_per_bar(bpm: float, beats_per_bar: int, sample_rate: int) -> float:
	return samples_per_beat(bpm, sample_rate) * float(beats_per_bar)


# First boundary sample at/after `request_sample` for `quantize`. Mirrors the
# Python `boundary_sample` (and the TD-11 predicted_boundary_sample formula).
static func boundary_sample(request_sample: int, quantize: String, bpm: float,
		beats_per_bar: int, sample_rate: int, length_bars: int = 0) -> int:
	match quantize:
		Q_IMMEDIATE:
			return request_sample
		Q_BEAT:
			var spbeat := samples_per_beat(bpm, sample_rate)
			return int(round(ceil(float(request_sample) / spbeat) * spbeat))
		Q_LOOP_END:
			var loop := samples_per_bar(bpm, beats_per_bar, sample_rate) * float(length_bars)
			return int(round(ceil(float(request_sample) / loop) * loop))
		_:  # Q_BAR
			var spb := samples_per_bar(bpm, beats_per_bar, sample_rate)
			return int(round(ceil(float(request_sample) / spb) * spb))


static func fade_samples_for(rule: Dictionary, bpm: float, beats_per_bar: int,
		sample_rate: int) -> int:
	if rule["fade_ms"] > 0.0:
		return int(round(float(rule["fade_ms"]) * sample_rate / 1000.0))
	return int(round(float(rule["fade_bars"]) * samples_per_bar(bpm, beats_per_bar, sample_rate)))


# --- crossfade schedule -------------------------------------------------
# Returns {from, to, request_sample, boundary_sample, fade_samples, quantize,
# stinger, ramps:[{layer,start_db,end_db,begin_sample,end_sample}]}. This is the
# plan the ZoneMusicPlayer executes and the TD-11 probe times against.
static func crossfade_schedule(from_state: String, to_state: String,
		request_sample: int, bpm: float, beats_per_bar: int, sample_rate: int,
		length_bars: int = 0) -> Dictionary:
	var rule := rule_for(from_state, to_state)
	var boundary := boundary_sample(request_sample, rule["quantize"], bpm,
		beats_per_bar, sample_rate, length_bars)
	var fade := fade_samples_for(rule, bpm, beats_per_bar, sample_rate)
	var from_gains := layer_gains_db(from_state)
	var to_gains := layer_gains_db(to_state)
	var ramps: Array = []
	for layer in LAYERS:
		var start_db: float = from_gains[layer]
		var end_db: float = to_gains[layer]
		if start_db == end_db:
			continue
		ramps.append({
			"layer": layer, "start_db": start_db, "end_db": end_db,
			"begin_sample": boundary, "end_sample": boundary + fade,
		})
	return {
		"from": from_state, "to": to_state, "request_sample": request_sample,
		"boundary_sample": boundary, "fade_samples": fade,
		"quantize": rule["quantize"], "stinger": rule["stinger"], "ramps": ramps,
	}
