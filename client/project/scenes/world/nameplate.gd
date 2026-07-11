# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — a floating NAMEPLATE (CMB-03, #535; combat-presentation epic #23):
# a billboarded name Label3D plus a thin health bar the world scene floats over a visible
# remote entity. The name comes from ENTITY_ENTER (0x2001); the health bar tracks
# current/max from the SAME server-authoritative vitals the unit frames read
# (VITALS_UPDATE 0x2004), routed through the event bus's entity_vitals_changed seam.
#
# PURE PRESENTATION — server-authoritative (Principle 1): this node NEVER derives a
# gameplay value. It only DISPLAYS the name + health/max the server sent. There is no
# client-side combat, faction, or hostility logic — ENTITY_ENTER carries no faction on the
# M1 wire, so the plate uses a NEUTRAL tint (the manager tints it the moment the payload
# grows a hostility field). The health bar green is the health readout, not a faction tint.
#
# BILLBOARDING is GPU-side (BaseMaterial3D.BILLBOARD_ENABLED on the label + both bar quads)
# so the plate faces the camera with NO per-frame CPU cost — it tracks the entity purely by
# being a child of the entity's Node3D (the world scene owns those nodes; the manager only
# attaches/recycles this plate over them, mirroring the chat-bubble / giver-marker pattern).
#
# The health bar is two unshaded, depth-test-off quads sharing ONE origin so they billboard
# identically and stay aligned at any camera angle: a full-width dark background and a fill
# whose QuadMesh shrinks from the RIGHT (via size + center_offset) so it empties left-anchored
# exactly like a 2D bar. Recolour/scale happen only on a vitals event, never per frame.

extends Node3D

# --- Layout (metres, in the entity's local space) ----------------------------
const BAR_WIDTH := 1.2       # full health-bar width over a ~1.8 m capsule
const BAR_HEIGHT := 0.16     # a THIN bar (WoW-style nameplate)
const BAR_Y := 2.15          # bar sits just above the head
const NAME_Y := 2.5          # name rides above the bar

# --- Neutral styling (no faction on the M1 wire → neutral) -------------------
const COL_NAME := Color(0.92, 0.92, 0.95)          # neutral light (readable on any tint)
const COL_BAR_BG := Color(0.05, 0.05, 0.06, 0.85)  # dark trough behind the fill
const COL_BAR_FILL := Color(0.30, 0.80, 0.35)      # health green (the health readout)
const NAME_PIXEL_SIZE := 0.006                     # Label3D world scale (metres per pixel)

var _name_label: Label3D
var _bar_bg: MeshInstance3D
var _bar_fill: MeshInstance3D
var _fill_mesh: QuadMesh
var _bg_mat: StandardMaterial3D
var _fill_mat: StandardMaterial3D

var _cur := 0        # last known current health (server-authoritative)
var _max := 0        # last known max health (0 = unknown → empty bar)
var _alpha := 1.0    # distance-fade alpha the manager drives


func _ready() -> void:
	# Name label — a billboarded, depth-test-off Label3D (matches the guid/chat labels).
	_name_label = Label3D.new()
	_name_label.name = "Name"
	_name_label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	_name_label.no_depth_test = true
	_name_label.fixed_size = false
	_name_label.pixel_size = NAME_PIXEL_SIZE
	_name_label.font_size = 36
	_name_label.outline_size = 8
	_name_label.outline_modulate = Color(0, 0, 0, 0.85)
	_name_label.modulate = COL_NAME
	_name_label.render_priority = 1
	_name_label.position = Vector3(0.0, NAME_Y, 0.0)
	add_child(_name_label)

	# Health bar — background trough + fill, sharing one origin so they billboard together.
	_bar_bg = _make_bar_quad(BAR_WIDTH, COL_BAR_BG, 0)
	_bar_bg.name = "HealthBarBg"
	_bar_bg.position = Vector3(0.0, BAR_Y, 0.0)
	add_child(_bar_bg)
	_bg_mat = _bar_bg.material_override

	_bar_fill = _make_bar_quad(BAR_WIDTH, COL_BAR_FILL, 1)
	_bar_fill.name = "HealthBarFill"
	# A hair in front of the trough — no_depth_test + render_priority already order them,
	# this just keeps the fill visually atop the background.
	_bar_fill.position = Vector3(0.0, BAR_Y, 0.001)
	add_child(_bar_fill)
	_fill_mesh = _bar_fill.mesh
	_fill_mat = _bar_fill.material_override

	_apply_health()
	_apply_alpha()


# Build one billboarded, unshaded, depth-test-off bar quad with its OWN QuadMesh + material
# (the fill mutates its mesh, so meshes must never be shared). `priority` orders fill over bg.
func _make_bar_quad(width: float, color: Color, priority: int) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = Vector2(width, BAR_HEIGHT)
	mi.mesh = q
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	m.billboard_keep_scale = true
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.no_depth_test = true
	m.render_priority = priority
	mi.material_override = m
	return mi


# --- Drive API (the manager -> plate) ----------------------------------------

# Set the displayed name (from ENTITY_ENTER; an empty string leaves the current text).
func set_name_text(text: String) -> void:
	if _name_label != null:
		_name_label.text = text


# Track the server's current/max health (ENTITY_ENTER baseline, then VITALS_UPDATE deltas).
func set_health(current: int, maximum: int) -> void:
	_cur = current
	_max = maximum
	_apply_health()


# Apply a distance-fade alpha in [0, 1] (the manager computes it from the camera distance).
func set_alpha(alpha_value: float) -> void:
	_alpha = clampf(alpha_value, 0.0, 1.0)
	_apply_alpha()


# --- Introspection (verification / debug) ------------------------------------

func health_ratio() -> float:
	if _max <= 0:
		return 0.0
	return clampf(float(_cur) / float(_max), 0.0, 1.0)

func name_text() -> String:
	return _name_label.text if _name_label != null else ""

func fill_width() -> float:
	return _fill_mesh.size.x if _fill_mesh != null else 0.0

func alpha() -> float:
	return _alpha


# --- Internals ---------------------------------------------------------------

# Shrink the fill quad from the RIGHT so the bar empties left-anchored: the fill spans
# [-W/2, -W/2 + W*ratio] regardless of ratio, exactly overlapping the trough's left edge.
func _apply_health() -> void:
	if _fill_mesh == null:
		return
	var ratio := health_ratio()
	_fill_mesh.size = Vector2(BAR_WIDTH * ratio, BAR_HEIGHT)
	_fill_mesh.center_offset = Vector3(-BAR_WIDTH * (1.0 - ratio) * 0.5, 0.0, 0.0)
	_bar_fill.visible = ratio > 0.0


# Fold the fade alpha into every element's base color (kept event-driven: only on set_alpha).
func _apply_alpha() -> void:
	if _name_label == null:
		return
	var nc := COL_NAME
	nc.a = _alpha
	_name_label.modulate = nc
	var bc := COL_BAR_BG
	bc.a = COL_BAR_BG.a * _alpha
	_bg_mat.albedo_color = bc
	var fc := COL_BAR_FILL
	fc.a = _alpha
	_fill_mat.albedo_color = fc
