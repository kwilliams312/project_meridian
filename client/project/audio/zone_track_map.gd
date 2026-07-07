# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — zone -> music-set -> stem resolution (ZoneMusicPlayer, #144).
#
# Engine-free (extends RefCounted). Loads the SAME config the Python reference
# `tools/zone_music/track_map.py` loads (client/project/audio/zone_music_config.json)
# so the mapping is data, never hardcoded (music SAD §6.1), and the two sides
# agree by construction. Content references stable asset IDs only (music PRD §7);
# the asset id -> runtime resource step (the #148 ID hook path) is done by the
# caller (ZoneMusicPlayer), keeping this file pure.
class_name ZoneTrackMap
extends RefCounted

const _VALID_LAYERS := ["L1", "L2", "L3", "L4", "boss", "stinger"]

var _config: Dictionary = {}
var _sets: Dictionary = {}  # set_id -> parsed set dict


static func from_file(path: String = "res://audio/zone_music_config.json") -> ZoneTrackMap:
	var text := FileAccess.get_file_as_string(path)
	assert(text != "", "zone_music_config.json not readable at %s" % path)
	var parsed: Variant = JSON.parse_string(text)
	assert(parsed is Dictionary, "zone_music_config.json is not a JSON object")
	return ZoneTrackMap.new(parsed)


func _init(config: Dictionary) -> void:
	_config = config
	_parse()


func _parse() -> void:
	var sets: Dictionary = _config.get("sets", {})
	for set_id in sets.keys():
		if String(set_id).begins_with("_"):
			continue
		_sets[set_id] = _parse_set(set_id, sets[set_id])
	assert(not _sets.is_empty(), "config defines no music sets")


func _parse_set(set_id: String, raw: Dictionary) -> Dictionary:
	var stems: Array = []
	var has_l1 := false
	for s in raw.get("stems", []):
		var layer := String(s["layer"])
		assert(_VALID_LAYERS.has(layer), "set %s: bad layer %s" % [set_id, layer])
		if layer == "L1":
			has_l1 = true
		stems.append({
			"layer": layer, "asset_id": String(s["asset_id"]),
			"placeholder_tone_hz": float(s.get("placeholder_tone_hz", 0.0)),
		})
	# Stem-set consistency (music SAD §4.4), same rule as the Python core.
	assert(has_l1, "set %s: no L1 bed stem (SAD §2.2)" % set_id)
	assert(stems.size() >= 5 and stems.size() <= 7,
		"set %s: %d stems, expected 5-7 (music PRD §2.1)" % [set_id, stems.size()])
	return {
		"set_id": set_id, "bpm": float(raw["bpm"]),
		"beats_per_bar": int(raw["beats_per_bar"]),
		"length_bars": int(raw["length_bars"]), "key": String(raw.get("key", "")),
		"stems": stems,
		"placeholder": bool(raw.get("placeholder", _config.get("placeholder", false))),
	}


func set_id_for_zone(zone_id: String) -> String:
	var zones: Dictionary = _config.get("zones", {})
	var entry: Variant = zones.get(zone_id)
	if entry == null:
		var default_zone: String = _config.get("default_zone", "")
		entry = zones.get(default_zone)
	assert(entry != null, "no music set mapped for zone %s and no default" % zone_id)
	return String((entry as Dictionary)["set"])


func default_state_for_zone(zone_id: String) -> String:
	var zones: Dictionary = _config.get("zones", {})
	var entry: Variant = zones.get(zone_id)
	if entry == null:
		entry = zones.get(_config.get("default_zone", ""), {})
	return String((entry as Dictionary).get("default_state", "explore"))


func music_set(set_id: String) -> Dictionary:
	assert(_sets.has(set_id), "unknown music set %s" % set_id)
	return _sets[set_id]


func resolve_zone(zone_id: String) -> Dictionary:
	return music_set(set_id_for_zone(zone_id))
