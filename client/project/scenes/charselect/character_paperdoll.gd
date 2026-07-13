# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — reusable character "paperdoll" preview widget (issue #639,
# char-select redesign). Factored out of char_select.gd's old in-code
# `_build_placeholder_preview` so BOTH the roster view (the selected character,
# front-facing) and the creation view (large, drag-to-rotate, live-updating) share
# ONE implementation.
#
# The widget IS a SubViewportContainer: it builds its own SubViewport + light +
# Camera3D + a `PreviewRoot` Node3D in code (the .tscn stays a plain Control tree,
# same convention as scenes/world/camera_demo.gd). The character model mounts under
# `PreviewRoot` as an AssembledCharacter (②/T4, #541) — the SAME assembly node the
# world builds — driven by (race, appearance). A content miss (no catalog for the
# race — spec §6) degrades to the tinted-capsule fallback, matching the "content
# missing" picker state. The mounted body is always named "PreviewBody" so callers
# and headless verifies can `find_child("PreviewBody")` regardless of which view
# owns the widget.
#
# DRAG-TO-ROTATE (#639): when `draggable` is true, a left-drag on the widget yaws
# `PreviewRoot` about Y. Yaw lives on the ROOT (not the mounted body), so a live
# re-assemble (set_appearance clears + rebuilds the body) preserves the current
# rotation. The roster view leaves `draggable` false (rotation optional there);
# the creation view sets it true.
#
# Preloaded by PATH (never the bare class name): the headless --script verify has no
# autoloads and a freshly-added class_name is invisible to a stale global class
# cache — preload is immune to both (same reason as AssembledCharacter/ContentDb).

extends SubViewportContainer

# AssembledCharacter (②/T4, #541) — preloaded by path (no global class cache in
# --script verify runs). Assemble() false → the capsule fallback (spec §6).
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")

# Camera framing (#643). A portrait-telephoto lens: a low FOV is flatter and more
# flattering than a wide angle, and the distance is chosen so a ~1.8 m standing figure
# (feet at y=0, head ~1.8) fills MOST of the frame head-to-toe with a modest margin,
# INDEPENDENT of the viewport's on-screen size (Godot's default KEEP_HEIGHT aspect fixes
# the vertical FOV, so the figure stays framed as the widget grows with the window, #630).
# At z=3.3 / fov=40 the visible vertical extent is ~2.4 m, so a 1.8 m figure fills ~75%.
const _CAM_FOV := 40.0
const _CAM_POS := Vector3(0.0, 0.98, 3.3)

## When true, a left-drag on the widget yaws the preview. Roster: false (front-facing);
## creation view: true. Set BEFORE or after add — read live in _gui_input.
var draggable: bool = false

var _viewport: SubViewport = null
var _preview_root: Node3D = null            # the model mounts under this; yaw lives here
var _yaw: float = 0.0                       # current drag yaw (radians), preserved across re-assembles
var _dragging: bool = false


func _init() -> void:
	# stretch: the SubViewportContainer keeps the SubViewport render surface sized to the
	# container, so under the project's `canvas_items` stretch the preview scales with the
	# window (#630). STOP mouse filter so _gui_input fires for the drag (the 3D sub-world
	# needs no picking).
	stretch = true
	mouse_filter = Control.MOUSE_FILTER_STOP


func _ready() -> void:
	_build()
	# Both call sites drop this widget into a PLAIN Control (`%PreviewHolder`), which lays
	# its children out by ANCHORS — not size flags (those only matter inside a Container).
	# Anchoring to the full parent rect makes the widget FILL its holder and grow with the
	# panel + window (#643 — previously it stayed pinned at custom_minimum_size, a small
	# model floating in a big empty panel). custom_minimum_size stays a floor, not a cap.
	# With `stretch = true` the SubViewportContainer then keeps the SubViewport RENDER size
	# matched to our real on-screen pixels, so the model is drawn LARGE and SHARP (never an
	# upscaled small surface) and re-tracks automatically on every resize.
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


# Build the SubViewport (light + camera) + the PreviewRoot the model mounts into.
# Idempotent (a lazy set_appearance before _ready builds it once, order-independent).
# The SubViewport size is NOT set here: `stretch = true` makes the SubViewportContainer
# own it and keep it matched to the container's real pixel size (#643).
func _build() -> void:
	if _preview_root != null:
		return

	_viewport = SubViewport.new()
	_viewport.transparent_bg = true
	add_child(_viewport)

	var world_root := Node3D.new()
	_viewport.add_child(world_root)

	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-45.0), deg_to_rad(35.0), 0.0)
	world_root.add_child(light)

	# The model mounts under this container so a re-assemble just clears + rebuilds it;
	# yaw lives on the root so rotation survives a live appearance change.
	_preview_root = Node3D.new()
	_preview_root.name = "PreviewRoot"
	_preview_root.rotation.y = _yaw
	world_root.add_child(_preview_root)

	# Framed for a standing figure (feet at y=0, head ~1.8) — tightened in #643 so the
	# model fills most of the viewport rather than floating small in a big margin.
	var cam := Camera3D.new()
	cam.position = _CAM_POS
	cam.fov = _CAM_FOV
	cam.current = true
	world_root.add_child(cam)


# Mount an AssembledCharacter for (race, appearance) into the preview, replacing
# whatever was there. On a content miss (no catalog for the race — assemble() false,
# spec §6) the tinted-capsule fallback stands in. Builds the viewport lazily if a
# caller drives it before _ready (order-independent).
func set_appearance(race: int, appearance: Dictionary, equipment: Array) -> void:
	if _preview_root == null:
		_build()
	for child in _preview_root.get_children():
		child.free()
	var assembled = AssembledCharacterScript.new()
	assembled.name = "PreviewBody"
	var ok: bool = assembled.assemble(race, 0, appearance, equipment)
	if ok:
		_preview_root.add_child(assembled)
		return
	assembled.free()
	_preview_root.add_child(_make_preview_capsule(
		int(appearance.get("skin", MeridianAppearance.DEFAULT_SKIN_ID))))


# The tinted-capsule fallback (D-11 placeholder), used when no catalog is mounted for
# the race (spec §6). Tinted by the chosen skin preset so the pick still reads (#435).
func _make_preview_capsule(skin_id: int) -> MeshInstance3D:
	var body := MeshInstance3D.new()
	body.name = "PreviewBody"
	var capsule := CapsuleMesh.new()
	capsule.height = 1.8
	capsule.radius = 0.35
	body.mesh = capsule
	body.position = Vector3(0, 0.9, 0)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = MeridianAppearance.skin_color(skin_id)
	body.material_override = mat
	# A small "nose" so the model has an obvious front (facing -Z, Godot forward).
	var nose := MeshInstance3D.new()
	var nm := BoxMesh.new()
	nm.size = Vector3(0.2, 0.2, 0.5)
	nose.mesh = nm
	nose.position = Vector3(0, 0.2, -0.45)
	body.add_child(nose)
	return body


# Drag-to-rotate: a left-drag yaws the preview root about Y (only when draggable).
func _gui_input(event: InputEvent) -> void:
	if not draggable:
		return
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		_dragging = event.pressed
	elif event is InputEventMouseMotion and _dragging:
		# Horizontal drag → yaw. Negative so dragging right spins the figure to face
		# the drag direction (natural "grab and turn").
		_yaw -= event.relative.x * 0.01
		if _preview_root != null:
			_preview_root.rotation.y = _yaw


## Current drag yaw in radians (for tests / callers that reset rotation).
func preview_yaw() -> float:
	return _yaw
