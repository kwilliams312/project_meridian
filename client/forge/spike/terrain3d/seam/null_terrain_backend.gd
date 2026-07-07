# NullTerrainBackend — an analytic terrain with ZERO Terrain3D dependency.
#
# Its only job in the spike is to PROVE the ITerrainBackend seam abstracts the
# backend: the exact same harness (spike_harness.gd) drives this and
# Terrain3DBackend through the identical interface, with no caller changes. It
# stands in for the "future in-house MeridianTerrainBackend" the A-09 seam
# exists to allow (Tools SAD §5.2) — if Terrain3D ever became untenable, a real
# backend drops in exactly here.
#
# Height is a deterministic ramp+ridge so exported heightfields are checkable by
# hand: h(x,z) = 0.05*x + 2*sin(z * 0.1).
class_name NullTerrainBackend
extends ITerrainBackend

const REGION_SIZE_128: int = 128
var _bound: Array[String] = []

func backend_name() -> String:
	return "NullTerrainBackend(analytic, no Terrain3D)"

func region_size_m() -> float:
	return float(REGION_SIZE_128)

func sculpt(_op: Dictionary) -> void:
	pass  # analytic surface; nothing to store

func paint(_op: Dictionary, _layer_slot: int) -> void:
	pass

func undo() -> void:
	pass

func redo() -> void:
	pass

func bind_layer(art_terrain_set_id: String) -> int:
	_bound.append(art_terrain_set_id)
	return _bound.size() - 1

func unbind_layer(layer_slot: int) -> void:
	if layer_slot >= 0 and layer_slot < _bound.size():
		_bound.remove_at(layer_slot)

func layer_capacity() -> int:
	return 8  # exactly the min budget — proves a lean backend still satisfies the seam

func export_heightfield(chunk: Vector2i) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(HEIGHTFIELD_SIDE * HEIGHTFIELD_SIDE)
	var origin := Vector2(chunk.x * CHUNK_SIZE_M, chunk.y * CHUNK_SIZE_M)
	for row in range(HEIGHTFIELD_SIDE):
		for col in range(HEIGHTFIELD_SIDE):
			var wx := origin.x + float(col)
			var wz := origin.y + float(row)
			out[row * HEIGHTFIELD_SIDE + col] = 0.05 * wx + 2.0 * sin(wz * 0.1)
	return out

func enumerate_collision_geometry(chunk: Vector2i, step_m: float = 4.0) -> PackedVector3Array:
	var verts := PackedVector3Array()
	var origin := Vector2(chunk.x * CHUNK_SIZE_M, chunk.y * CHUNK_SIZE_M)
	var n := int(CHUNK_SIZE_M / step_m)
	for gz in range(n + 1):
		for gx in range(n + 1):
			var wx := origin.x + gx * step_m
			var wz := origin.y + gz * step_m
			verts.append(Vector3(wx, 0.05 * wx + 2.0 * sin(wz * 0.1), wz))
	return verts
