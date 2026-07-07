@tool
extends EditorNode3DGizmoPlugin
## ForgeZoneMarkerGizmo — the M0 skeleton's ONE 3D gizmo (issue #134).
##
## Draws a ForgeZoneMarker's zone-bounds box (a `size_m` × `size_m` XZ footprint,
## `height_m` tall) plus a small origin cross, in the editor viewport. This is the
## EditorNode3DGizmoPlugin half of the plugin-architecture proof (Tools SAD §5.1:
## "custom node classes + EditorNode3DGizmoPlugin per type"). Real Forge gizmos
## (leash radius, patrol waypoints, volume extents) follow this same shape in M1.

const MARKER_SCRIPT := preload("res://addons/meridian_forge/nodes/forge_zone_marker.gd")

const MAT_BOUNDS := "forge_zone_bounds"
const MAT_ORIGIN := "forge_zone_origin"


func _init() -> void:
	create_material(MAT_BOUNDS, Color(0.20, 0.85, 0.45))  # zone-bounds wireframe
	create_material(MAT_ORIGIN, Color(1.0, 0.75, 0.15))   # origin cross


func _get_gizmo_name() -> String:
	return "ForgeZoneMarker"


func _has_gizmo(node: Node3D) -> bool:
	return node != null and node.get_script() == MARKER_SCRIPT


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()

	var node := gizmo.get_node_3d()
	if node == null:
		return

	var half := float(node.size_m) * 0.5
	var h := float(node.height_m)

	# Eight corners of the bounds box (XZ footprint centred on the node origin,
	# rising from y=0 to y=height_m).
	var c := [
		Vector3(-half, 0.0, -half), Vector3(half, 0.0, -half),
		Vector3(half, 0.0, half), Vector3(-half, 0.0, half),
		Vector3(-half, h, -half), Vector3(half, h, -half),
		Vector3(half, h, half), Vector3(-half, h, half),
	]

	var lines := PackedVector3Array()
	# Bottom ring, top ring, verticals.
	for pair in [[0, 1], [1, 2], [2, 3], [3, 0],
			[4, 5], [5, 6], [6, 7], [7, 4],
			[0, 4], [1, 5], [2, 6], [3, 7]]:
		lines.push_back(c[pair[0]])
		lines.push_back(c[pair[1]])
	gizmo.add_lines(lines, get_material(MAT_BOUNDS, gizmo), false)

	# A small origin cross so the zone origin (SAD §3.1 zone-local coords) is visible.
	var cross_size := maxf(1.0, half * 0.08)
	var cross := PackedVector3Array([
		Vector3(-cross_size, 0.0, 0.0), Vector3(cross_size, 0.0, 0.0),
		Vector3(0.0, 0.0, -cross_size), Vector3(0.0, 0.0, cross_size),
		Vector3(0.0, 0.0, 0.0), Vector3(0.0, cross_size, 0.0),
	])
	gizmo.add_lines(cross, get_material(MAT_ORIGIN, gizmo), false)
