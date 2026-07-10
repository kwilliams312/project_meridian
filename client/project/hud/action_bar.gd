# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the ACTION BAR view (CMB-01/UI-01, D-10, #432): a row of ability
# slots (keybinds 1..N) with an optimistic GCD/cooldown sweep. Pressing a slot (mouse or
# its number key) issues a CAST_REQUEST for that ability against the current target.
#
# PURE VIEW / MVVM (the #431 contract): owns NO server state and NO net handle. It renders
# the greybox known-ability set the event bus holds (MeridianEventBus.ability_set_changed —
# seeded from hud/ability_set.gd because the wire has no KNOWN_ABILITIES push, see that
# file) and a slot press becomes a bus INTENT (request_cast); the world scene turns that
# into a CAST_REQUEST frame. The GCD sweep is driven by the bus's D-10 optimistic-GCD
# window (gcd_changed): the sweep starts on the press and ROLLS BACK cleanly when the
# server rejects (the bus clears the window from CAST_FAILED.gcd_remaining_ms).
#
# PERFORMANCE (≤2 ms / no per-frame alloc, #431 epic exit criterion): idle, this view does
# ZERO work — no _process. _process is enabled ONLY while a GCD is actually sweeping (~1.5 s)
# and disabled the instant it elapses; the sweep is a LINEAR drain (a ColorRect height write
# per slot), so even the transient animation allocates nothing on the hot path.
#
# Built in code (like unit_frame.gd / trainer_window.gd) — self-contained, no .tscn.
class_name MeridianActionBar
extends Control

const AbilitySet := preload("res://hud/ability_set.gd")

const SLOT := 52.0        # slot square edge (px)
const GAP := 6.0          # gap between slots

var _bus: MeridianEventBus
var _row: HBoxContainer
var _error: Label         # transient CAST_FAILED reason flash
var _slots: Array = []    # per-slot view records (see _add_slot)
var _abilities: Array = []
var _built := false


func _ready() -> void:
	_build()
	set_process(false)  # idle until a GCD is live


# Bind to the world session's event bus (called once by the HUD). Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.ability_set_changed.connect(_on_ability_set_changed)
	_bus.gcd_changed.connect(_on_gcd_changed)
	_bus.cast_failed_reason.connect(_on_cast_failed_reason)
	# Paint the initial set (the bus may already hold a seeded greybox set).
	_on_ability_set_changed(_bus.abilities())


# Fire slot `index` (0-based) — the keybind path the world scene calls for keys 1..N.
# Same effect as clicking the slot. No-op for an out-of-range index.
func press_slot(index: int) -> void:
	if index < 0 or index >= _slots.size():
		return
	_fire(index)


func _build() -> void:
	if _built:
		return
	_row = HBoxContainer.new()
	_row.add_theme_constant_override("separation", int(GAP))
	add_child(_row)

	# Transient error flash sits just above the row (CAST_FAILED reason).
	_error = Label.new()
	_error.position = Vector2(0.0, -22.0)
	_error.add_theme_color_override("font_color", Color(1.0, 0.5, 0.45))
	_error.add_theme_color_override("font_outline_color", Color.BLACK)
	_error.add_theme_constant_override("outline_size", 4)
	_error.visible = false
	add_child(_error)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_ability_set_changed(abilities: Array) -> void:
	if not _built:
		_build()
	_abilities = abilities
	_rebuild_slots()


func _on_gcd_changed(_start_ms: int, duration_ms: int) -> void:
	if duration_ms > 0:
		set_process(true)   # sweep until the window elapses
	else:
		_clear_sweeps()     # cleared (a clean D-10 rollback) — snap slots ready + idle
		set_process(false)


func _on_cast_failed_reason(_ability_id: int, reason: int) -> void:
	_flash_error(_reason_text(reason))


# --- Per-frame GCD sweep (only while a GCD is live) --------------------------

func _process(_delta: float) -> void:
	if _bus == null:
		set_process(false)
		return
	var dur := _bus.gcd_duration_ms()
	var rem := _bus.gcd_remaining_ms(Time.get_ticks_msec())
	if dur <= 0 or rem <= 0:
		_clear_sweeps()
		set_process(false)
		return
	_apply_sweep(float(rem) / float(dur))


# --- Rendering ---------------------------------------------------------------

func _rebuild_slots() -> void:
	for s in _slots:
		(s["root"] as Control).queue_free()
	_slots.clear()
	for i in range(_abilities.size()):
		_add_slot(i, _abilities[i] as Dictionary)


func _add_slot(index: int, a: Dictionary) -> void:
	var ability_id := int(a.get("ability_id", 0))
	var hotkey := int(a.get("hotkey", index + 1))
	var icon_id := int(a.get("icon_id", ability_id))
	var ability_name := String(a.get("name", ""))

	var root := Control.new()
	root.custom_minimum_size = Vector2(SLOT, SLOT)

	# Placeholder icon swatch (the art/ID hook — a stable color from icon_id, mirroring how
	# the trainer window renders an ability by id until real icon art ships).
	var swatch := ColorRect.new()
	swatch.color = _placeholder_color(icon_id)
	swatch.position = Vector2.ZERO
	swatch.size = Vector2(SLOT, SLOT)
	swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(swatch)

	# Ability id label (centered) — greybox "#<id>" identity.
	var idl := Label.new()
	idl.text = "#%d" % ability_id
	idl.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	idl.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	idl.position = Vector2.ZERO
	idl.size = Vector2(SLOT, SLOT)
	idl.add_theme_color_override("font_color", Color.WHITE)
	idl.add_theme_color_override("font_outline_color", Color.BLACK)
	idl.add_theme_constant_override("outline_size", 3)
	idl.add_theme_font_size_override("font_size", 13)
	idl.mouse_filter = Control.MOUSE_FILTER_IGNORE
	idl.tooltip_text = ability_name
	root.add_child(idl)

	# Hotkey number (top-left).
	var key := Label.new()
	key.text = str(hotkey)
	key.position = Vector2(3.0, 1.0)
	key.add_theme_color_override("font_color", Color(1.0, 0.95, 0.7))
	key.add_theme_color_override("font_outline_color", Color.BLACK)
	key.add_theme_constant_override("outline_size", 3)
	key.add_theme_font_size_override("font_size", 12)
	key.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(key)

	# GCD/cooldown sweep overlay — a dark LINEAR drain covering the top `ratio` of the slot
	# (full on press, shrinking to nothing when ready). Zero-alloc: only its height changes.
	var sweep := ColorRect.new()
	sweep.color = Color(0.0, 0.0, 0.0, 0.55)
	sweep.position = Vector2.ZERO
	sweep.size = Vector2(SLOT, 0.0)
	sweep.visible = false
	sweep.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(sweep)

	# Transparent click target on top (mouse press path; keys go through press_slot()).
	var btn := Button.new()
	btn.flat = true
	btn.position = Vector2.ZERO
	btn.size = Vector2(SLOT, SLOT)
	btn.custom_minimum_size = Vector2(SLOT, SLOT)
	btn.focus_mode = Control.FOCUS_NONE
	btn.pressed.connect(func() -> void: _fire(index))
	root.add_child(btn)

	_row.add_child(root)
	_slots.append({"root": root, "sweep": sweep, "ability_id": ability_id})


# Issue the CAST_REQUEST intent for slot `index` (0-based). The bus predicts the GCD
# optimistically + emits the intent (dropping the press if a predicted GCD is still live).
func _fire(index: int) -> void:
	if _bus == null or index < 0 or index >= _slots.size():
		return
	var ability_id := int((_slots[index] as Dictionary)["ability_id"])
	_bus.request_cast(ability_id, Time.get_ticks_msec())
	# gcd_changed (emitted by request_cast on a fired press) enables _process — no work here.


func _apply_sweep(ratio: float) -> void:
	var h := SLOT * clampf(ratio, 0.0, 1.0)
	var show := h > 0.5
	for s in _slots:
		var sw := (s["sweep"] as ColorRect)
		sw.size = Vector2(SLOT, h)
		sw.visible = show


func _clear_sweeps() -> void:
	for s in _slots:
		var sw := (s["sweep"] as ColorRect)
		sw.size = Vector2(SLOT, 0.0)
		sw.visible = false


func _flash_error(text: String) -> void:
	if _error == null:
		return
	_error.text = text
	_error.visible = true
	# Auto-hide after a short beat (SceneTreeTimer is headless-safe).
	var tree := get_tree()
	if tree == null:
		return
	var t := tree.create_timer(1.6)
	t.timeout.connect(func() -> void:
		if is_instance_valid(_error):
			_error.visible = false)


# A stable placeholder swatch color from an icon/ability id (greybox art hook): spread the
# id around the hue wheel so adjacent slots read as distinct without any real icon art.
static func _placeholder_color(icon_id: int) -> Color:
	var hue := fmod(float(icon_id) * 0.6180339887, 1.0)  # golden-ratio hue spread
	return Color.from_hsv(hue, 0.45, 0.42, 1.0)


# world.fbs CastFailReason → a short readable flash (mirrors the trainer/loot reason maps).
static func _reason_text(reason: int) -> String:
	match reason:
		0: return "Unknown ability."
		1: return "Not in world."
		2: return "You are dead."
		3: return "Not ready yet."          # ON_GCD
		4: return "Already casting."
		5: return "Not enough resource."
		6: return "You have no target."
		7: return "Target is dead."
		8: return "Cannot attack that."
		9: return "Out of range."
		10: return "Not in line of sight."
		11: return "Interrupted."
		_: return "You cannot do that yet."
