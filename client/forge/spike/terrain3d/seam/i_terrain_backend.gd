# ITerrainBackend — GDScript realization of the A-09 swap seam (Tools SAD §5.2).
#
# The SAD carries a C++ header sketch for this interface but marks it "DESIGN
# SKETCH, not compiled" — the first *real* embodiment is here, in GDScript,
# because Forge's docks / chunk exporter / navmesh bake are the GDScript editor
# plugin that actually calls it (Tools SAD §5.1, §5.4). forge_core (C++) can
# implement the same five operations later without changing callers.
#
# This is the whole point of A-09: every Forge caller talks to THIS interface,
# never to Terrain3D's API directly, so the backend stays swappable. Two
# implementations live beside this file:
#   * Terrain3DBackend  — the vendored Terrain3D fork (the A-09 choice)
#   * NullTerrainBackend — an analytic flat/ramp backend, proving swappability
#                          with zero Terrain3D dependency
#
# Operation set is the five from Tools SAD §5.2 (1:1 with the C++ sketch):
#   1. sculpt / paint / undo / redo
#   2. paint-layer <-> art.* terrain-set binding (+ capacity)
#   3. 128 m region-alignment query
#   4. export_heightfield(chunk) -> f32[129x129]
#   5. collision-relevant geometry enumeration (navmesh bake input)
class_name ITerrainBackend
extends RefCounted

# Per-chunk heightfield shape, fixed by Tools SAD §3.2 / §5.2:
# 129x129 row-major float32, 1 m spacing, 128 m span + shared edge.
const HEIGHTFIELD_SIDE: int = 129
const CHUNK_SIZE_M: float = 128.0
# Sentinel for holes / un-sculpted cells. export_heightfield must surface these
# (a lint at export) rather than silently zeroing (Tools SAD §5.2 op 4).
const HOLE_SENTINEL: float = NAN

# --- (1) sculpt / paint -------------------------------------------------------
# op: {center: Vector3, radius: float, strength: float, mode: int}
func sculpt(_op: Dictionary) -> void:
	_abstract("sculpt")

func paint(_op: Dictionary, _layer_slot: int) -> void:
	_abstract("paint")

func undo() -> void:
	_abstract("undo")

func redo() -> void:
	_abstract("redo")

# --- (2) paint-layer <-> art.* terrain-set binding ----------------------------
# Returns the assigned slot, or -1 if over the zone budget (Art PRD §2.3).
func bind_layer(_art_terrain_set_id: String) -> int:
	_abstract("bind_layer")
	return -1

func unbind_layer(_layer_slot: int) -> void:
	_abstract("unbind_layer")

# Backends expose >= the budget (~8/zone). Terrain3D == 32.
func layer_capacity() -> int:
	_abstract("layer_capacity")
	return 0

# --- (3) 128 m region-alignment query ----------------------------------------
func region_size_m() -> float:
	_abstract("region_size_m")
	return 0.0

# region_size_m() tiles the 128 m chunk grid (Tools SAD §3.2).
func aligns_to_chunk_grid() -> bool:
	return is_equal_approx(fmod(CHUNK_SIZE_M, region_size_m()), 0.0) \
		or is_equal_approx(fmod(region_size_m(), CHUNK_SIZE_M), 0.0)

# --- (4) server heightfield extraction ---------------------------------------
# chunk: Vector2i(cx, cz). Returns a 129*129 PackedFloat32Array, row-major,
# zone-local metres, 1 m spacing. Holes come back as HOLE_SENTINEL.
func export_heightfield(_chunk: Vector2i) -> PackedFloat32Array:
	_abstract("export_heightfield")
	return PackedFloat32Array()

# --- (5) collision-relevant geometry for the shared Recast bake --------------
# Returns a flat vertex stream (PackedVector3Array, triangles) over the chunk,
# terrain-walkable-tagged, for the Recast pipeline. Decoupled from (4): Recast
# may want a coarser sampling than the server movement grid (SAD §5.2 op 5).
func enumerate_collision_geometry(_chunk: Vector2i, _step_m: float = 4.0) -> PackedVector3Array:
	_abstract("enumerate_collision_geometry")
	return PackedVector3Array()

func backend_name() -> String:
	return "ITerrainBackend(abstract)"

func _abstract(method: String) -> void:
	push_error("ITerrainBackend.%s() is abstract — use a concrete backend." % method)
