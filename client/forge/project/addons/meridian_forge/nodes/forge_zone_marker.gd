@tool
extends Node3D
## ForgeZoneMarker — the M0 skeleton's ONE custom Node3D type (issue #134).
##
## Stands in for the real Forge spatial nodes (KitInstance, MeridianSpawnPoint,
## MeridianPatrolPath, MeridianVolume — Tools SAD §5.1) that land in M1. Its only
## job here is to be a registered custom type that carries a 128 m zone-bounds box
## the gizmo (gizmos/forge_zone_marker_gizmo.gd) draws in the viewport — proving the
## custom-node + EditorNode3DGizmoPlugin half of the plugin architecture.
##
## The bounds default to one Meridian chunk (128 m, SAD §3.2). Editing `size_m` in
## the inspector redraws the gizmo (property change -> update_gizmos()).

## Side length, in metres, of the (square, XZ) zone-bounds box this marker shows.
## Defaults to one 128 m chunk. Clamped positive.
@export var size_m: float = 128.0:
	set(value):
		size_m = maxf(0.001, value)
		update_gizmos()

## Height, in metres, of the drawn bounds box (visual aid only).
@export var height_m: float = 32.0:
	set(value):
		height_m = maxf(0.001, value)
		update_gizmos()
