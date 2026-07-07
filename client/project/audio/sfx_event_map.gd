# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — gameplay/UI event -> SFX-asset-id -> resource resolution
# (SFX trigger runtime, #148).
#
# Engine-free (extends RefCounted). Loads the SAME config the Python reference
# `tools/zone_sfx/event_map.py` loads (client/project/audio/sfx_config.json) so
# the mapping is data, never hardcoded (music SAD §5), and the two sides agree by
# construction. Content references stable asset IDs only (music PRD §7); the
# asset id -> runtime resource step (the #148 ID hook path) is done by the caller
# (SfxPlayer), keeping this file pure.
#
# The SFX counterpart of zone_track_map.gd (#144): music resolves zone->set->
# stem, SFX resolves event->sfx-id->one-shot, through the SAME ID hook.
class_name SfxEventMap
extends RefCounted

const _VALID_ATTENUATION := ["small", "medium", "large", "global", "ui2d"]
# `sfx.*` id, optionally namespaced — mirrors `sfxRef` in common.defs.yaml.
const _SFX_ID_RE := "^([a-z][a-z0-9_]{1,31}:)?sfx\\.[a-z0-9_]+(\\.[a-z0-9_]+)*$"

var _config: Dictionary = {}
var _categories: Dictionary = {}  # name -> {bus, group, cap}
var _sfx: Dictionary = {}         # sfx_id -> {category, attenuation, ...}
var _events: Dictionary = {}      # event_key -> sfx_id
var _id_regex: RegEx


static func from_file(path: String = "res://audio/sfx_config.json") -> SfxEventMap:
	var text := FileAccess.get_file_as_string(path)
	assert(text != "", "sfx_config.json not readable at %s" % path)
	var parsed: Variant = JSON.parse_string(text)
	assert(parsed is Dictionary, "sfx_config.json is not a JSON object")
	return SfxEventMap.new(parsed)


func _init(config: Dictionary) -> void:
	_config = config
	_id_regex = RegEx.new()
	_id_regex.compile(_SFX_ID_RE)
	_parse()


func _parse() -> void:
	var cats: Dictionary = _config.get("categories", {})
	for name in cats.keys():
		if String(name).begins_with("_"):
			continue
		var raw: Dictionary = cats[name]
		_categories[name] = {
			"bus": String(raw["bus"]), "group": String(raw["group"]),
			"cap": int(raw["cap"]),
		}
	assert(not _categories.is_empty(), "config defines no SFX categories")

	var sfx: Dictionary = _config.get("sfx", {})
	for sfx_id in sfx.keys():
		if String(sfx_id).begins_with("_"):
			continue
		_sfx[sfx_id] = _parse_sfx(sfx_id, sfx[sfx_id])
	assert(not _sfx.is_empty(), "config defines no SFX entries")

	var events: Dictionary = _config.get("events", {})
	for event_key in events.keys():
		if String(event_key).begins_with("_"):
			continue
		var sfx_id := String(events[event_key])
		assert(_sfx.has(sfx_id), "event %s -> unknown sfx id %s" % [event_key, sfx_id])
		_events[event_key] = sfx_id


func _parse_sfx(sfx_id: String, raw: Dictionary) -> Dictionary:
	assert(_id_regex.search(sfx_id) != null,
		"sfx id %s is not a valid sfx.* id (an ID, never a path)" % sfx_id)
	var category := String(raw["category"])
	assert(_categories.has(category), "sfx %s: unknown category %s" % [sfx_id, category])
	var attenuation := String(raw["attenuation"])
	assert(_VALID_ATTENUATION.has(attenuation),
		"sfx %s: bad attenuation %s" % [sfx_id, attenuation])
	return {
		"sfx_id": sfx_id, "category": category, "attenuation": attenuation,
		"placeholder_tone_hz": float(raw.get("placeholder_tone_hz", 0.0)),
		"placeholder_ms": int(raw.get("placeholder_ms", 0)),
	}


func placeholder() -> bool:
	return bool(_config.get("placeholder", false))


func sfx_ids() -> Array:
	return _sfx.keys()


func sfx_entry(sfx_id: String) -> Dictionary:
	assert(_sfx.has(sfx_id), "unknown sfx id %s" % sfx_id)
	return _sfx[sfx_id]


func category(name: String) -> Dictionary:
	assert(_categories.has(name), "unknown SFX category %s" % name)
	return _categories[name]


func resolve_event(event_key: String) -> String:
	assert(_events.has(event_key), "no SFX mapped for event %s" % event_key)
	return _events[event_key]


func footstep_event(surface: String) -> String:
	# physics-material -> surface tag -> foley footstep variation group (SAD §5).
	return "footstep.%s" % surface


func bus_for(sfx_id: String) -> String:
	return String(category(sfx_entry(sfx_id)["category"])["bus"])


func group_for(sfx_id: String) -> String:
	return String(category(sfx_entry(sfx_id)["category"])["group"])


# True if `sfx_id` is a syntactically valid sfx.* id — used by the content hook
# so an ability's cast_sfx/impact_sfx resolves without a config `sfx` entry.
func is_valid_sfx_id(sfx_id: String) -> bool:
	return _id_regex.search(sfx_id) != null
