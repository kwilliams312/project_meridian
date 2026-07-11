# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the DEATH / GHOST / RESURRECT overlay (CMB-03, #359/#532): the
# full-screen presentation of the death→ghost→corpse-run→rez loop.
#
# PURE VIEW / MVVM (the #431 contract): owns NO server state and NO net handle. It binds to
# MeridianEventBus's death-loop signals — `died` opens the death overlay (a dim screen, "You
# have died", the auto-release countdown, and a Release control); `became_ghost` switches to
# the ghost presentation (a greyscale screen tint + corpse-run guidance + a Resurrect control);
# `corpse_distance_changed` updates the guidance and enables Resurrect within range; a
# `resurrected` OK clears the overlay (restores the normal, colored view) while a refusal keeps
# the ghost up and surfaces the reason. The client NEVER predicts the phase — it only presents
# what the server sent and emits the two intents (Release / Resurrect) through the bus.
#
# PERFORMANCE (#431): idle (alive), ZERO work — no _process. _process runs only while the death
# countdown is live (a once-per-frame label update), and stops the instant the player releases
# or resurrects.
#
# Built in code (like unit_frame.gd / cast_bar.gd) — self-contained, no .tscn.
class_name MeridianDeathOverlay
extends Control

# Greyscale screen-space desaturation for the ghost view: a canvas_item shader that samples the
# framebuffer behind this node and writes back its luminance. Drawn UNDER the overlay's own
# labels/buttons (added later), so the guidance text stays legible/colored over the grey world.
const _GHOST_SHADER := """
shader_type canvas_item;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear;
void fragment() {
	vec3 c = texture(screen_tex, SCREEN_UV).rgb;
	float g = dot(c, vec3(0.299, 0.587, 0.114));
	COLOR = vec4(vec3(g), 1.0);
}
"""

var _bus: MeridianEventBus
var _built := false

var _dim: ColorRect            # dark vignette shown while dead (pre-release)
var _grey: ColorRect           # greyscale screen desaturation shown while a ghost
var _panel: VBoxContainer      # centered title/status/button stack
var _title: Label
var _status: Label
var _release_btn: Button
var _resurrect_btn: Button

var _release_deadline_ms := 0  # Time.get_ticks_msec() stamp the auto-release fires at


func _ready() -> void:
	_build()
	_hide_all()


# Bind to the world session's event bus (called once by the HUD). Idempotent per bus.
func setup(bus: MeridianEventBus) -> void:
	if bus == null or _bus == bus:
		return
	_bus = bus
	_bus.died.connect(_on_died)
	_bus.became_ghost.connect(_on_became_ghost)
	_bus.corpse_distance_changed.connect(_on_corpse_distance_changed)
	_bus.resurrected.connect(_on_resurrected)
	# Paint the initial state (the bus may already hold a death/ghost phase from queued frames).
	var phase := _bus.death_phase()
	if phase == MeridianEventBus.DEATH_PHASE_DEAD:
		_on_died(_bus.corpse_guid(), _bus.corpse_position(), _bus.auto_release_ms())
	elif phase == MeridianEventBus.DEATH_PHASE_GHOST:
		_on_became_ghost(_bus.graveyard_position(), _bus.corpse_position(), _bus.corpse_guid())


func _build() -> void:
	if _built:
		return
	# Cover the whole screen and let clicks reach the buttons; the container is centered.
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE

	# Greyscale desaturation (ghost). Full-rect, added FIRST so labels/buttons draw over it.
	_grey = ColorRect.new()
	_grey.name = "GhostGreyscale"
	_grey.set_anchors_preset(Control.PRESET_FULL_RECT)
	_grey.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var sh := Shader.new()
	sh.code = _GHOST_SHADER
	var mat := ShaderMaterial.new()
	mat.shader = sh
	_grey.material = mat
	add_child(_grey)

	# Dim vignette (dead, pre-release). A translucent black wash so the world reads as "downed".
	_dim = ColorRect.new()
	_dim.name = "DeathDim"
	_dim.color = Color(0.0, 0.0, 0.0, 0.55)
	_dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	_dim.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_dim)

	# Centered title / status / buttons stack.
	_panel = VBoxContainer.new()
	_panel.name = "DeathPanel"
	_panel.set_anchors_preset(Control.PRESET_CENTER)
	_panel.grow_horizontal = Control.GROW_DIRECTION_BOTH
	_panel.grow_vertical = Control.GROW_DIRECTION_BOTH
	_panel.alignment = BoxContainer.ALIGNMENT_CENTER
	_panel.add_theme_constant_override("separation", 14)
	add_child(_panel)

	_title = Label.new()
	_title.name = "DeathTitle"
	_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_title.add_theme_color_override("font_color", Color(0.92, 0.30, 0.30))
	_title.add_theme_color_override("font_outline_color", Color.BLACK)
	_title.add_theme_constant_override("outline_size", 4)
	_title.add_theme_font_size_override("font_size", 34)
	_panel.add_child(_title)

	_status = Label.new()
	_status.name = "DeathStatus"
	_status.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_status.add_theme_color_override("font_color", Color.WHITE)
	_status.add_theme_color_override("font_outline_color", Color.BLACK)
	_status.add_theme_constant_override("outline_size", 3)
	_status.add_theme_font_size_override("font_size", 16)
	_panel.add_child(_status)

	_release_btn = Button.new()
	_release_btn.name = "ReleaseButton"
	_release_btn.text = "Release Spirit"
	_release_btn.custom_minimum_size = Vector2(200, 36)
	_release_btn.pressed.connect(_on_release_pressed)
	_panel.add_child(_release_btn)

	_resurrect_btn = Button.new()
	_resurrect_btn.name = "ResurrectButton"
	_resurrect_btn.text = "Resurrect"
	_resurrect_btn.custom_minimum_size = Vector2(200, 36)
	_resurrect_btn.pressed.connect(_on_resurrect_pressed)
	_panel.add_child(_resurrect_btn)

	_built = true


# --- Bus signal handlers -----------------------------------------------------

func _on_died(_corpse_guid: int, _corpse_position: Vector3, auto_release_ms: int) -> void:
	if not _built:
		_build()
	_grey.visible = false
	_dim.visible = true
	_release_btn.visible = true
	_resurrect_btn.visible = false
	_title.text = "You have died"
	visible = true
	if auto_release_ms > 0:
		_release_deadline_ms = Time.get_ticks_msec() + auto_release_ms
		_update_countdown()
		set_process(true)
	else:
		_release_deadline_ms = 0
		_status.text = "Release your spirit to the graveyard."
		set_process(false)


func _on_became_ghost(_graveyard_position: Vector3, _corpse_position: Vector3, _corpse_guid: int) -> void:
	if not _built:
		_build()
	set_process(false)  # the release countdown is over
	_release_deadline_ms = 0
	_dim.visible = false
	_grey.visible = true
	_release_btn.visible = false
	_resurrect_btn.visible = true
	_resurrect_btn.disabled = true  # enabled once the corpse-run reaches range
	_title.text = "You are a ghost"
	_status.text = "Run to your corpse to resurrect."
	visible = true


func _on_corpse_distance_changed(distance: float) -> void:
	if not _built or not _bus.is_ghost():
		return
	if distance <= MeridianEventBus.RESURRECT_RANGE_M:
		_resurrect_btn.disabled = false
		_status.text = "You have reached your corpse. Resurrecting…"
	else:
		_resurrect_btn.disabled = true
		_status.text = "Run to your corpse — %.0f m away." % distance


func _on_resurrected(status: int, _health: int, _max_health: int) -> void:
	if status == MeridianEventBus.RESURRECT_OK:
		set_process(false)
		_hide_all()
		return
	# Refused — still a ghost. Surface the typed reason and re-arm the Resurrect button.
	if not _built:
		_build()
	_resurrect_btn.disabled = false
	_status.text = _refusal_text(status)


# --- Button intent handlers --------------------------------------------------

func _on_release_pressed() -> void:
	if _bus != null:
		_bus.request_release()


func _on_resurrect_pressed() -> void:
	if _bus != null:
		_bus.request_resurrect()


# --- Countdown ---------------------------------------------------------------

func _process(_delta: float) -> void:
	if _release_deadline_ms <= 0:
		set_process(false)
		return
	_update_countdown()


func _update_countdown() -> void:
	var remaining_ms := _release_deadline_ms - Time.get_ticks_msec()
	if remaining_ms <= 0:
		# Auto-release fires server-side; wait for the GHOST_STATE to switch presentation.
		_status.text = "Releasing to the graveyard…"
		set_process(false)
		return
	_status.text = "Auto-release in %.0f s — or release now." % ceil(float(remaining_ms) / 1000.0)


# --- Internals ---------------------------------------------------------------

func _hide_all() -> void:
	visible = false
	if _dim != null:
		_dim.visible = false
	if _grey != null:
		_grey.visible = false


static func _refusal_text(status: int) -> String:
	match status:
		1: return "You are not dead."                       # RESURRECT_NOT_DEAD
		2: return "Release to the graveyard first."          # RESURRECT_NOT_RELEASED
		3: return "Too far from your corpse — move closer."  # RESURRECT_TOO_FAR
		_: return "You cannot resurrect right now."


# --- Test/read accessors (headless verify) -----------------------------------

## Whether the overlay is currently shown (dead or ghost).
func is_overlay_visible() -> bool:
	return visible

## The current title text (e.g. "You have died" / "You are a ghost").
func title_text() -> String:
	return _title.text if _title != null else ""

## The current status/guidance text (countdown / corpse-run distance / refusal reason).
func status_text() -> String:
	return _status.text if _status != null else ""

## Whether the Release control is shown (dead, pre-release).
func is_release_visible() -> bool:
	return _release_btn != null and _release_btn.visible

## Whether the Resurrect control is shown AND pressable (ghost within corpse range).
func is_resurrect_enabled() -> bool:
	return _resurrect_btn != null and _resurrect_btn.visible and not _resurrect_btn.disabled

## Whether the greyscale ghost desaturation is active.
func is_ghost_view() -> bool:
	return _grey != null and _grey.visible
