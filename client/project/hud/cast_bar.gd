# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the CAST BAR view (CMB-01/UI-01, D-10, #432): a progress bar that
# fills over a cast-time ability's cast, driven entirely by the server's timing.
#
# PURE VIEW / MVVM (the #431 contract): owns NO server state and NO net handle. It binds to
# MeridianEventBus.cast_bar_changed — the bus opens the bar on CAST_START (with the server's
# cast_ms) and CLOSES it on CAST_RESULT (cast completed) or CAST_FAILED / INTERRUPTED (the
# cast was rejected mid-flight). The client NEVER predicts the cast's completion or outcome
# (Principle 1) — it only animates the fill between the server's start and the server's
# resolution.
#
# PERFORMANCE (≤2 ms / no per-frame alloc, #431): idle, ZERO work — no _process. _process is
# enabled only while a cast is in flight (fill = a ColorRect width write), and disabled the
# instant the bus clears the cast.
#
# Built in code (like unit_frame.gd) — self-contained, no .tscn.
class_name MeridianCastBar
extends Control

const BAR_W := 260.0
const BAR_H := 20.0

var _bus: MeridianEventBus
var _bg: ColorRect
var _fill: ColorRect
var _label: Label
var _start_ms := 0
var _duration_ms := 0
var _built := false


func _ready() -> void:
	_build()
	set_frame_visible(false)
	set_process(false)


# Bind to the world session's event bus (called once by the HUD). Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.cast_bar_changed.connect(_on_cast_bar_changed)
	# Paint the initial state (the bus may already hold an active cast).
	if _bus.cast_active():
		_on_cast_bar_changed(true, _bus.cast_ability_id(), _bus.cast_start_ms(),
			_bus.cast_duration_ms())


func set_frame_visible(v: bool) -> void:
	visible = v


func _build() -> void:
	if _built:
		return
	custom_minimum_size = Vector2(BAR_W, BAR_H)

	_bg = ColorRect.new()
	_bg.color = Color(0.08, 0.08, 0.10, 0.9)
	_bg.position = Vector2.ZERO
	_bg.size = Vector2(BAR_W, BAR_H)
	add_child(_bg)

	_fill = ColorRect.new()
	_fill.color = Color(1.0, 0.82, 0.35)  # warm "casting" amber
	_fill.position = Vector2.ZERO
	_fill.size = Vector2(0.0, BAR_H)
	add_child(_fill)

	_label = Label.new()
	_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_label.position = Vector2.ZERO
	_label.size = Vector2(BAR_W, BAR_H)
	_label.add_theme_color_override("font_color", Color.WHITE)
	_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_label.add_theme_constant_override("outline_size", 3)
	_label.add_theme_font_size_override("font_size", 12)
	add_child(_label)

	_built = true


# --- Bus signal handler ------------------------------------------------------

func _on_cast_bar_changed(active: bool, ability_id: int, start_ms: int, duration_ms: int) -> void:
	if not _built:
		_build()
	if active and duration_ms > 0:
		_start_ms = start_ms
		_duration_ms = duration_ms
		_label.text = "Casting  #%d" % ability_id
		_fill.size = Vector2(0.0, BAR_H)
		set_frame_visible(true)
		set_process(true)
	else:
		set_process(false)
		set_frame_visible(false)


func _process(_delta: float) -> void:
	if _duration_ms <= 0:
		set_process(false)
		return
	var elapsed := Time.get_ticks_msec() - _start_ms
	var ratio := clampf(float(elapsed) / float(_duration_ms), 0.0, 1.0)
	_fill.size = Vector2(BAR_W * ratio, BAR_H)
	# Hold at full when the fill completes; the bus closes the bar on CAST_RESULT (the
	# server-authoritative resolution) — the client never self-completes the cast.
	if ratio >= 1.0:
		set_process(false)
