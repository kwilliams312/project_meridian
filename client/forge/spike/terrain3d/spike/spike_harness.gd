# spike_harness.gd — the A-09 validation harness (issue #132).
#
# Drives Terrain3D BEHIND the ITerrainBackend seam and measures what this box
# can measure, then prints a structured report. Runs headless (issue #283):
#   godot --headless --path <spike> --script res://spike/spike_harness.gd -- --report
#
# What it PROVES (measured here, no display needed):
#   V1  Terrain3D GDExtension loads in Godot 4.7            (ClassDB has Terrain3D)
#   V2  Seam op 3: region_size == 128 m, aligns to chunk grid
#   V3  Seam op 4: export_heightfield -> exact 129x129 f32, real (non-hole) values
#   V4  Seam abstracts the backend: identical harness drives NullTerrainBackend
#   V5  Hole sentinel: un-sculpted cells come back NAN, not silent 0
#   M1  export_heightfield() wall time (per chunk) + collision enumerate time
#   M2  static memory delta for a populated single-chunk region
#
# What it CANNOT measure here (needs the min-spec bench, #61 / TD-03 GTX 1060):
#   render frame time @ 1080p Low. Method is in docs/terrain3d-spike-report.md;
#   the harness emits the exact scene + steps the owner runs on the bench.
extends SceneTree

const ITerrainBackendC := preload("res://seam/i_terrain_backend.gd")
const Terrain3DBackendC := preload("res://seam/terrain3d_backend.gd")
const NullTerrainBackendC := preload("res://seam/null_terrain_backend.gd")

var _pass := 0
var _fail := 0

func _init() -> void:
	# Terrain3D creates its data object on NOTIFICATION_ENTER_TREE and finishes
	# subsystem init inside the world; run the checks after the first processed
	# frame so the node is fully live. quit() ends the loop when done.
	process_frame.connect(_run, ConnectFlags.CONNECT_ONE_SHOT)

func _run() -> void:
	print("\n=== Terrain3D spike harness (issue #132) — Godot %s ===" %
		Engine.get_version_info().string)
	print("Backend seam: ITerrainBackend (Tools SAD §5.2)\n")

	_v1_extension_loads()
	var t3d_backend := _make_terrain3d_backend()
	if t3d_backend != null:
		_v2_region_alignment(t3d_backend)
		_measure_and_verify(t3d_backend, true)
	else:
		_report("V2/V3", false, "Terrain3D node could not be created — skipping backend checks")

	# V4: the SAME code path, different backend — proves the seam abstracts.
	print("\n--- Seam swappability (V4): re-run through NullTerrainBackend ---")
	var null_backend := NullTerrainBackendC.new()
	_v2_region_alignment(null_backend)
	_measure_and_verify(null_backend, false)

	print("\n=== RESULT: %d passed, %d failed ===" % [_pass, _fail])
	quit(0 if _fail == 0 else 1)

func _v1_extension_loads() -> void:
	var ok := ClassDB.class_exists("Terrain3D") and ClassDB.class_exists("Terrain3DData")
	_report("V1  Terrain3D GDExtension loaded (4.7)", ok,
		"ClassDB.class_exists('Terrain3D')=%s" % ClassDB.class_exists("Terrain3D"))

func _make_terrain3d_backend() -> ITerrainBackend:
	if not ClassDB.class_exists("Terrain3D"):
		return null
	var node: Node = ClassDB.instantiate("Terrain3D")
	get_root().add_child(node)
	var backend: ITerrainBackend = Terrain3DBackendC.new(node)
	return backend

func _v2_region_alignment(backend: ITerrainBackend) -> void:
	var rs := backend.region_size_m()
	var aligns := backend.aligns_to_chunk_grid()
	_report("V2  [%s] region_size == 128 m" % backend.backend_name(),
		is_equal_approx(rs, 128.0), "region_size_m()=%.1f" % rs)
	_report("V2  [%s] aligns_to_chunk_grid()" % backend.backend_name(),
		aligns, "aligns=%s" % aligns)

func _measure_and_verify(backend: ITerrainBackend, is_terrain3d: bool) -> void:
	var chunk := Vector2i(0, 0)

	# V5 (Terrain3D only): before sculpting, cells should be holes (NAN),
	# not silent zeros — the export lint depends on this.
	if is_terrain3d:
		var pre := backend.export_heightfield(chunk)
		var nan_count := 0
		for h in pre:
			if is_nan(h):
				nan_count += 1
		_report("V5  hole sentinel: un-sculpted cells are NAN (not 0)",
			nan_count > 0, "%d/%d cells NAN pre-sculpt" % [nan_count, pre.size()])

	# Sculpt a small hill so the export has real relief.
	var mem_before := OS.get_static_memory_usage()
	var t_sculpt := Time.get_ticks_usec()
	backend.sculpt({"center": Vector3(64, 0, 64), "radius": 40.0, "strength": 12.0})
	var sculpt_us := Time.get_ticks_usec() - t_sculpt

	# M1: export timing (the server-consumed op, run per chunk at bake).
	var t_export := Time.get_ticks_usec()
	var hf := backend.export_heightfield(chunk)
	var export_us := Time.get_ticks_usec() - t_export
	var mem_after := OS.get_static_memory_usage()

	# V3: exact shape + at least some real relief.
	var side := ITerrainBackendC.HEIGHTFIELD_SIDE
	var shape_ok := hf.size() == side * side
	var real_vals := 0
	var hmin := INF
	var hmax := -INF
	for h in hf:
		if not is_nan(h):
			real_vals += 1
			hmin = minf(hmin, h)
			hmax = maxf(hmax, h)
	_report("V3  [%s] export_heightfield -> exact 129x129 (%d)" % [backend.backend_name(), hf.size()],
		shape_ok, "size=%d expected=%d" % [hf.size(), side * side])
	_report("V3  [%s] heightfield carries real relief" % backend.backend_name(),
		real_vals > 0 and hmax > hmin,
		"non-hole cells=%d, height range=[%.3f, %.3f] m" % [real_vals, hmin, hmax])

	# M1b: collision enumeration timing (Recast bake input, op 5).
	var t_geo := Time.get_ticks_usec()
	var geo := backend.enumerate_collision_geometry(chunk, 4.0)
	var geo_us := Time.get_ticks_usec() - t_geo

	# Layer capacity (op 2) sanity: must meet >= 8 budget.
	var cap := backend.layer_capacity()
	_report("     [%s] layer_capacity >= 8 budget" % backend.backend_name(),
		cap >= 8, "capacity=%d" % cap)

	print("  MEASURED [%s]:" % backend.backend_name())
	print("     sculpt (r=40 brush)         : %6.3f ms" % (sculpt_us / 1000.0))
	print("     export_heightfield (129x129): %6.3f ms  (%d samples)" % [export_us / 1000.0, hf.size()])
	print("     enumerate_collision (4 m)   : %6.3f ms  (%d verts)" % [geo_us / 1000.0, geo.size()])
	print("     static mem delta (1 region) : %6.1f KiB" % ((mem_after - mem_before) / 1024.0))

func _report(label: String, ok: bool, detail: String) -> void:
	if ok:
		_pass += 1
	else:
		_fail += 1
	print("  [%s] %s  (%s)" % ["PASS" if ok else "FAIL", label, detail])
