# Terrain3DBackend — the A-09 chosen backend, wrapping the vendored Terrain3D
# fork behind ITerrainBackend (Tools SAD §5.2). NOTHING outside this file
# touches a Terrain3D.* symbol; that is what keeps the choice reversible.
#
# Maps the five seam operations onto Terrain3D's real GDExtension API
# (method names verified against the vendored src/*.cpp bindings):
#   region_size (change_region_size/get_region_size), vertex_spacing,
#   get_data() -> Terrain3DData{ set_height, get_height, add_region_blankp,
#   get_region, get_height_map }.
class_name Terrain3DBackend
extends ITerrainBackend

# Terrain3D region-size enum value for a 128 m region (Terrain3D.SIZE_128).
# One region == exactly one Meridian chunk at 1 m vertex spacing (terrain-eval C1).
const REGION_SIZE_128: int = 128
const LAYER_CAPACITY: int = 32  # Terrain3D ships 32 texture slots (terrain-eval C3).

var _terrain: Node = null            # Terrain3D node (typed as Node to avoid a
                                     # hard class ref if the extension is absent).
var _data: Object = null             # Terrain3DData
var _bound_layers: Array[String] = []

# region_origin_m: world-space min-corner of the chunk the spike operates on.
func _init(terrain_node: Node) -> void:
	_terrain = terrain_node
	assert(_terrain != null, "Terrain3DBackend needs a Terrain3D node")
	# Pin the 128 m region size so a region == one chunk (C1). This is the single
	# config line the region-alignment query (op 3) asserts on.
	if _terrain.has_method("change_region_size"):
		_terrain.change_region_size(REGION_SIZE_128)
	_terrain.set("vertex_spacing", 1.0)  # 1 m/vertex -> 129x129 per 128 m region.
	# Terrain3D creates its Terrain3DData on ENTER_TREE (_initialize); the node
	# must already be in the tree when the backend is built.
	if _terrain.has_method("get_data"):
		_data = _terrain.get_data()
	assert(_data != null, "Terrain3D data is null — add the Terrain3D node to the tree before wrapping it")

func backend_name() -> String:
	return "Terrain3DBackend(vendored fork)"

# --- (3) region alignment -----------------------------------------------------
func region_size_m() -> float:
	if _terrain.has_method("get_region_size"):
		return float(_terrain.get_region_size())
	return float(_terrain.get("region_size"))

# --- (1) sculpt: raise/lower/set within a brush radius ------------------------
# Minimal real height edit through Terrain3D's set_height; enough to prove the
# sculpt op writes into the terrain the exporter later reads. (Forge's full
# brush stack — smooth/flatten + undo bridging — layers on top of this.)
func sculpt(op: Dictionary) -> void:
	var center: Vector3 = op.get("center", Vector3.ZERO)
	var radius: float = op.get("radius", 8.0)
	var strength: float = op.get("strength", 1.0)
	_ensure_region_at(center)
	var r := int(ceil(radius))
	for dz in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var d := Vector2(dx, dz).length()
			if d > radius:
				continue
			var falloff := 1.0 - (d / radius)
			var p := center + Vector3(dx, 0.0, dz)
			var h: float = _data.get_height(p)
			if is_nan(h):
				h = 0.0
			_data.set_height(p, h + strength * falloff)

func paint(_op: Dictionary, _layer_slot: int) -> void:
	# Paint writes the control map; not exercised in the headless heightfield
	# spike (needs a material + texture set). Left as a real no-op so the seam
	# contract is complete. Editor-UX paint is a visual the owner confirms.
	pass

func undo() -> void:
	pass  # Forge bridges Terrain3D's editor undo stack; out of scope headless.

func redo() -> void:
	pass

# --- (2) layer binding --------------------------------------------------------
func bind_layer(art_terrain_set_id: String) -> int:
	if _bound_layers.size() >= LAYER_CAPACITY:
		return -1
	_bound_layers.append(art_terrain_set_id)
	return _bound_layers.size() - 1

func unbind_layer(layer_slot: int) -> void:
	if layer_slot >= 0 and layer_slot < _bound_layers.size():
		_bound_layers.remove_at(layer_slot)

func layer_capacity() -> int:
	return LAYER_CAPACITY

# --- (4) heightfield export ---------------------------------------------------
# 129x129 @ 1 m over the chunk, row-major, zone-local metres. Uses the
# backend-agnostic get_height() lattice-sampling path (terrain-eval C4 path 2):
# robust, exact at 1 m spacing since Terrain3D stores native float32 height.
func export_heightfield(chunk: Vector2i) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(HEIGHTFIELD_SIDE * HEIGHTFIELD_SIDE)
	var origin := Vector3(chunk.x * CHUNK_SIZE_M, 0.0, chunk.y * CHUNK_SIZE_M)
	for row in range(HEIGHTFIELD_SIDE):
		for col in range(HEIGHTFIELD_SIDE):
			var p := origin + Vector3(float(col), 0.0, float(row))
			var h: float = _data.get_height(p)
			# Terrain3D returns NAN for holes / outside regions — pass the
			# sentinel straight through so the exporter can lint it (op 4).
			out[row * HEIGHTFIELD_SIDE + col] = h
	return out

# --- (5) collision geometry ---------------------------------------------------
func enumerate_collision_geometry(chunk: Vector2i, step_m: float = 4.0) -> PackedVector3Array:
	var verts := PackedVector3Array()
	var origin := Vector3(chunk.x * CHUNK_SIZE_M, 0.0, chunk.y * CHUNK_SIZE_M)
	var n := int(CHUNK_SIZE_M / step_m)
	for gz in range(n):
		for gx in range(n):
			var p00 := origin + Vector3(gx * step_m, 0.0, gz * step_m)
			var p10 := p00 + Vector3(step_m, 0.0, 0.0)
			var p01 := p00 + Vector3(0.0, 0.0, step_m)
			var p11 := p00 + Vector3(step_m, 0.0, step_m)
			for p in [p00, p10, p11, p00, p11, p01]:
				var s: Vector3 = p
				s.y = _sample(p)
				verts.append(s)
	return verts

func _sample(p: Vector3) -> float:
	var h: float = _data.get_height(p)
	return 0.0 if is_nan(h) else h

func _ensure_region_at(global_pos: Vector3) -> void:
	if _data.has_method("has_regionp") and _data.has_regionp(global_pos):
		return
	if _data.has_method("add_region_blankp"):
		_data.add_region_blankp(global_pos, true)
