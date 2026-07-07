# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — client Settings service (issue #108).
#
# The GDScript face of the `Settings` autoload singleton (Client SAD §2.1:
# "Autoload singletons (Net, Sim, Datastore, EventBus, Settings) wrap the
# GDExtension modules"). It is a thin convenience layer over the C++
# `MeridianSettings` node (client/gdextension/meridian/src/meridian_settings.*),
# which itself wraps the tested engine-free settings + first-run auto-benchmark
# core (settings_core.*). ALL policy — the typed schema/defaults, INI persistence,
# corrupt-file self-heal, the benchmark->quality-tier mapping, and the first-run-
# once decision — lives in that engine-free C++ core; this file only orchestrates
# boot and re-exposes accessors for the settings UI.
#
# ⚠ NOT YET AUTOLOADED. Consistent with the repo's discipline that unverified
# CLIENT code must not land wired into the boot flow (the M0 client cannot be
# verified headlessly — the MoltenVK/Apple-Silicon caveat #283; see
# scenes/charselect/character_store.gd for the same stance), this service is
# provided but NOT registered in project.godot [autoload]. Wiring it as the
# `Settings` autoload + building the M1 settings UI panel (PRD §11: graphics tiers
# §2.2, audio, keybinds, interface) is the scoped follow-up, done once the client
# gains a warm-boot verification path. Until then, a scene that wants settings can
# instantiate this node directly.
#
# Usage once wired (or via direct instantiation):
#   var summary := Settings.boot()          # loads user://settings.cfg; first run -> benchmark
#   var tier := Settings.quality_tier()     # MeridianSettings.TIER_LOW/MEDIUM/HIGH/EPIC
#   Settings.set_bool("graphics", "vsync", false)
#   Settings.save()

extends Node
class_name SettingsService

## The C++ settings node (typed store + user:// I/O + first-run benchmark seam).
var _settings: MeridianSettings = null

## The Dictionary returned by the last boot()/load — status, first_run, tier, etc.
var last_boot_result: Dictionary = {}


func _ensure() -> void:
	if _settings == null:
		_settings = MeridianSettings.new()


## Load user://settings.cfg; on a first launch (or a corrupt file) run the first-run
## auto-benchmark SKELETON, pick a starting quality tier, and persist it so it never
## re-runs. Returns the summary Dictionary (see MeridianSettings::load_or_initialize).
## Safe to call once at boot; idempotent thereafter (a loaded file does not re-bench).
func boot() -> Dictionary:
	_ensure()
	last_boot_result = _settings.load_or_initialize()
	if bool(last_boot_result.get("first_run", false)):
		print("[Settings] first run: benchmark(%s) score=%.0f -> tier '%s'" % [
			last_boot_result.get("benchmark_method", ""),
			float(last_boot_result.get("benchmark_score", 0.0)),
			last_boot_result.get("quality_tier_name", "?"),
		])
	else:
		print("[Settings] loaded (%s), tier '%s'" % [
			last_boot_result.get("status", "?"),
			last_boot_result.get("quality_tier_name", "?"),
		])
	return last_boot_result


## Persist the current settings to user://settings.cfg. Call after the settings UI
## commits changes. Returns true on success.
func save() -> bool:
	_ensure()
	return _settings.save()


## Current quality tier (MeridianSettings.TIER_*).
func quality_tier() -> int:
	_ensure()
	return _settings.get_quality_tier()


## Override the quality tier (settings-UI preset picker). Persist with save().
func set_quality_tier(tier: int) -> void:
	_ensure()
	_settings.set_quality_tier(tier)


## Typed accessors — pass-throughs to the C++ node (unknown key / type mismatch is
## rejected there and returns the fallback, never coerces).
func get_bool(section: String, key: String, fallback: bool = false) -> bool:
	_ensure()
	return _settings.get_bool(section, key, fallback)


func get_int(section: String, key: String, fallback: int = 0) -> int:
	_ensure()
	return _settings.get_int(section, key, fallback)


func get_float(section: String, key: String, fallback: float = 0.0) -> float:
	_ensure()
	return _settings.get_float(section, key, fallback)


func get_string(section: String, key: String, fallback: String = "") -> String:
	_ensure()
	return _settings.get_string(section, key, fallback)


func set_bool(section: String, key: String, value: bool) -> bool:
	_ensure()
	return _settings.set_bool(section, key, value)


func set_int(section: String, key: String, value: int) -> bool:
	_ensure()
	return _settings.set_int(section, key, value)


func set_float(section: String, key: String, value: float) -> bool:
	_ensure()
	return _settings.set_float(section, key, value)


func set_string(section: String, key: String, value: String) -> bool:
	_ensure()
	return _settings.set_string(section, key, value)


## Every current value as {section: {key: value}} — for a data-driven settings UI.
func as_dictionary() -> Dictionary:
	_ensure()
	return _settings.as_dictionary()


## Restore every knob to its schema default (settings-UI "restore defaults").
func reset_to_defaults() -> void:
	_ensure()
	_settings.reset_to_defaults()
