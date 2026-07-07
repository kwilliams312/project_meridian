extends SceneTree
## Headless verification of the Forge skeleton bridge (issue #134).
##
## Proves — WITHOUT the editor UI — that the forge_core GDExtension loaded and the
## ForgeCore class is registered and callable across the plugin↔native boundary.
## The dock UI + the viewport gizmo are visual and confirmed by the owner in the
## editor; THIS is the headless half.
##
## Run (after seeding .godot/ via a one-time windowed import, #283/#290):
##   Godot --headless --path client/forge/project --script res://forge_verify.gd
## Exits 0 on success, 1 on any failed assertion.

func _initialize() -> void:
	var failures := 0

	if not ClassDB.class_exists("ForgeCore"):
		printerr("FAIL: ForgeCore not registered — forge_core extension did not load")
		quit(1)
		return
	print("PASS: ForgeCore registered in ClassDB")

	var core: Object = ClassDB.instantiate("ForgeCore")
	if core == null:
		printerr("FAIL: ForgeCore.instantiate() returned null")
		quit(1)
		return
	print("PASS: ForgeCore instantiated")

	var version: String = core.version()
	if version.find("forge_core") == -1 or version.find("4.7") == -1:
		printerr("FAIL: unexpected version string: ", version)
		failures += 1
	else:
		print("PASS: version() = ", version)

	# ITerrainBackend region-alignment seam (SAD §5.2 op 3).
	var info: Dictionary = core.terrain_backend_info()
	print("INFO: terrain_backend_info = ", info)
	if not is_equal_approx(float(info.get("region_size_m", 0.0)), 128.0):
		printerr("FAIL: region_size_m != 128"); failures += 1
	else:
		print("PASS: region_size_m == 128 (Terrain3D SIZE_128 pin)")
	if int(info.get("heightfield_side", 0)) != 129:
		printerr("FAIL: heightfield_side != 129"); failures += 1
	else:
		print("PASS: heightfield_side == 129")
	if not bool(info.get("aligns", false)):
		printerr("FAIL: 128 m region does not align to the 128 m chunk grid"); failures += 1
	else:
		print("PASS: region aligns to the 128 m chunk grid")

	# Direct seam predicate: a 100 m region must NOT tile the 128 m grid.
	if core.region_tiles_chunk_grid(100.0):
		printerr("FAIL: 100 m region wrongly reported as tiling"); failures += 1
	else:
		print("PASS: 100 m region correctly rejected by region_tiles_chunk_grid()")

	if failures == 0:
		print("forge_verify: ALL CHECKS PASSED")
		quit(0)
	else:
		printerr("forge_verify: ", failures, " check(s) FAILED")
		quit(1)
