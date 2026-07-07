# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — SfxPlayer v0 (SFX trigger runtime seed, issue #148).
#
# The client-side SFX trigger runtime (music SAD §5). Given a gameplay/UI event
# (or a content-carried sfx id) it resolves the event -> `sfx.*` id -> one-shot
# through the content-ID hook and plays it on the routed bus (SAD §2.5 bus tree).
# Built once at M0, parameterized forever after.
#
# M0 scope (this file):
#   * event -> sfx-id resolution via the engine-free `SfxEventMap`;
#   * the #148 ID hook path: sfx id -> resource. At M0 the resolver is the
#     PLACEHOLDER factory (synthesized one-shot), NOT a loaded audio file;
#   * category -> bus routing over the SFX / UI bus tree (SAD §2.5);
#   * a small pool of `AudioStreamPlayer`s so a trigger never churns the scene
#     tree; last-trigger telemetry so the path is observable headlessly.
#
# NOT in M0 scope (deferred to M1, per SAD §2.5 / §7): the voice manager
# (concurrency-group caps, priority eviction, own-action guarantee, occlusion)
# and spatial `AudioStreamPlayer3D` attenuation presets. The `cap` in the config
# is carried as data for that work but is not enforced here.
#
# Client-API contract (SAD §5): gameplay/UI code calls `play_event()` /
# `play_sfx()` / `play_content_sfx()`; no audio object ever sees a network
# message. Real `sfx.*` assets resolve through the #148 ID hook path when the
# audio-direction work (#143) lands; until then the placeholder factory fills it.
#
# The SFX counterpart of zone_music_player.gd (#144).
class_name SfxPlayer
extends Node

const SfxEventMapScript := preload("res://audio/sfx_event_map.gd")
const SfxPlaceholderFactory := preload("res://audio/sfx_placeholder_factory.gd")

const _CONFIG_PATH := "res://audio/sfx_config.json"
const _POOL_SIZE := 8  # M0: fixed small pool; the M1 voice manager owns caps.

# The SFX/UI bus tree (music SAD §2.5). Child buses send to their SFX parent so a
# single "SFX" master volume (A-06 settings) governs all children; UI is separate
# (never occluded/attenuated).
const _SFX_CHILD_BUSES := ["SFX_Combat", "SFX_Foley", "SFX_NPC", "SFX_World"]

signal sfx_triggered(sfx_id: String, bus: String)

var _event_map  # SfxEventMap (untyped: resolved via preload, not the global)
var _pool: Array[AudioStreamPlayer] = []
var _next_player: int = 0
# Last-trigger telemetry, so headless tests can observe the resolved path.
var _last_trigger: Dictionary = {}


func _ready() -> void:
	if _event_map == null:
		configure()


# Load the event->sfx config. Kept separate from _ready() so headless tests can
# inject a config path before the node enters the tree.
func configure(config_path: String = _CONFIG_PATH) -> void:
	_event_map = SfxEventMapScript.from_file(config_path)
	_ensure_buses()
	if _pool.is_empty():
		for _i in range(_POOL_SIZE):
			var p := AudioStreamPlayer.new()
			add_child(p)
			_pool.append(p)


# Ensure the SFX/UI bus tree exists (SAD §2.5). The canonical layout is
# Client-owned (A-06); until a default_bus_layout ships we create it under Master
# so the player is self-contained and headlessly testable.
func _ensure_buses() -> void:
	_ensure_bus("SFX", "Master")
	_ensure_bus("UI", "Master")
	for child in _SFX_CHILD_BUSES:
		_ensure_bus(child, "SFX")


func _ensure_bus(name: String, parent: String) -> void:
	if AudioServer.get_bus_index(name) != -1:
		return
	var idx := AudioServer.bus_count
	AudioServer.add_bus(idx)
	AudioServer.set_bus_name(idx, name)
	var send := parent if AudioServer.get_bus_index(parent) != -1 else "Master"
	AudioServer.set_bus_send(idx, send)


# --- public Client API (SAD §5) -----------------------------------------
# Play a client-authored event (footstep.<surface>, ui.click, login.sting, ...).
func play_event(event_key: String) -> String:
	var sfx_id := String(_event_map.resolve_event(event_key))
	play_sfx(sfx_id)
	return sfx_id


# Play a footstep for a physics-surface tag (SAD §5).
func play_footstep(surface: String) -> String:
	return play_event(_event_map.footstep_event(surface))


# Play a config-declared sfx id directly.
func play_sfx(sfx_id: String) -> void:
	var entry: Dictionary = _event_map.sfx_entry(sfx_id)
	var bus := String(_event_map.bus_for(sfx_id))
	# ID hook path (#148): asset id -> resource. At M0 the resolver is the
	# PLACEHOLDER factory (synthesized one-shot), NOT a loaded sfx file.
	var stream := SfxPlaceholderFactory.resolve_sfx(
		sfx_id, float(entry["placeholder_tone_hz"]), int(entry["placeholder_ms"]))
	_emit(sfx_id, bus, stream)


# Play a CONTENT-carried sfx id (an ability's cast_sfx/impact_sfx, an NPC
# sound_set) through the SAME hook. Such ids live in /content, not in this
# config's `sfx` table, so they carry no placeholder synth params — the M0
# placeholder is a neutral tone. When #143 lands, both paths load real assets.
func play_content_sfx(sfx_id: String, bus: String = "SFX") -> void:
	assert(_event_map.is_valid_sfx_id(sfx_id), "not a valid sfx.* id: %s" % sfx_id)
	var stream := SfxPlaceholderFactory.resolve_sfx(sfx_id, 300.0, 120)
	_emit(sfx_id, bus, stream)


# --- playback -----------------------------------------------------------
func _emit(sfx_id: String, bus: String, stream: AudioStream) -> void:
	var p := _pool[_next_player]
	_next_player = (_next_player + 1) % _pool.size()
	p.bus = bus
	p.stream = stream
	p.play()
	_last_trigger = {"sfx_id": sfx_id, "bus": bus,
		"resource_name": stream.resource_name}
	sfx_triggered.emit(sfx_id, bus)


func last_trigger() -> Dictionary:
	return _last_trigger.duplicate()


func event_map():  # -> SfxEventMap
	return _event_map
