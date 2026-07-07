@tool
extends EditorPlugin
## Meridian Forge — EditorPlugin entry point (issue #134, M0 skeleton).
##
## Proves the Forge plugin architecture end-to-end with the smallest real slice
## (Tools SAD §5.1 / §8 M0 exit; PRD R3): on enable this plugin
##   1. adds ONE dock panel (ZoneDock, a Control) to the editor, and that dock
##      calls into the forge_core GDExtension (ForgeCore.version() + the
##      ITerrainBackend region-alignment seam) — proving the plugin↔native bridge;
##   2. registers ONE custom Node3D type (ForgeZoneMarker) so it appears in
##      Create Node and can be dropped into a 3D scene; and
##   3. registers ONE 3D gizmo (ForgeZoneMarkerGizmo) that draws the marker's
##      128 m zone-bounds wireframe + origin in the viewport.
##
## Everything heavier (real terrain ops, kit placement, spawn/patrol/volume nodes,
## chunk export, Recast bake) is later Forge work (SAD §5.1–§5.4); this file is the
## registration skeleton those build on.

const ZoneDock := preload("res://addons/meridian_forge/docks/zone_dock.gd")
const ForgeZoneMarker := preload("res://addons/meridian_forge/nodes/forge_zone_marker.gd")
const ForgeZoneMarkerGizmo := preload("res://addons/meridian_forge/gizmos/forge_zone_marker_gizmo.gd")

const CUSTOM_TYPE_NAME := "ForgeZoneMarker"

var _dock: Control = null
var _gizmo_plugin: EditorNode3DGizmoPlugin = null


func _enter_tree() -> void:
	# (3) 3D gizmo — register BEFORE the custom type so a marker created immediately
	# after picks up its gizmo.
	_gizmo_plugin = ForgeZoneMarkerGizmo.new()
	add_node_3d_gizmo_plugin(_gizmo_plugin)

	# (2) Custom Node3D type — shows up in the Create-Node dialog.
	add_custom_type(CUSTOM_TYPE_NAME, "Node3D", ForgeZoneMarker, null)

	# (1) Dock panel — a Control that bridges into forge_core.
	_dock = ZoneDock.new()
	_dock.name = "Forge"
	add_control_to_dock(EditorPlugin.DOCK_SLOT_LEFT_UR, _dock)

	print("[meridian_forge] enabled: dock + ForgeZoneMarker type + gizmo registered")


func _exit_tree() -> void:
	if _dock != null:
		remove_control_from_docks(_dock)
		_dock.free()
		_dock = null

	remove_custom_type(CUSTOM_TYPE_NAME)

	if _gizmo_plugin != null:
		remove_node_3d_gizmo_plugin(_gizmo_plugin)
		_gizmo_plugin = null

	print("[meridian_forge] disabled: dock + type + gizmo removed")


func _get_plugin_name() -> String:
	return "Meridian Forge"
