# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — rendering-backend boot verification (issue #115).
#
# Boots the SAME world test map (camera_demo.tscn — the scene char-select enters,
# res://scenes/world/camera_demo.tscn) under whatever rendering driver Godot was
# launched with (`--rendering-driver metal` or `--rendering-driver vulkan`),
# proves it reaches the map on a REAL GPU device, records the active backend, and
# writes a screenshot as evidence. This is the automatable counterpart to the
# interactive `scripts/dev/run-client.sh --renderer=<metal|vulkan>` boot.
#
# It is deliberately WINDOWED (a real Metal/MoltenVK device) — the `--headless`
# editor/import path aborts inside MoltenVK's SPIRVToMSLConverter on macOS
# (issue #283), so a real-device boot is the only reliable way to exercise either
# backend end-to-end. See client/README.md "Rendering backends".
#
# Run (Metal — the shipped default; Vulkan/MoltenVK — the diagnostic fallback):
#   godot --rendering-driver metal  --path project \
#         --script res://scenes/world/renderer_boot_verify.gd --quit-after 120
#   godot --rendering-driver vulkan --path project \
#         --script res://scenes/world/renderer_boot_verify.gd --quit-after 120
#
# Requires the compiled GDExtension (client/project/bin/) and a seeded `.godot/`
# (run-client.sh seeds it windowed once — never headless, per #283). Exits 0 on
# success, 1 on any failed assertion.

extends SceneTree

const TEST_MAP := "res://scenes/world/camera_demo.tscn"

var _fails := 0
var _frame := 0
var _reported := false


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian renderer-backend boot verify (#115)")

	# --- The test map resource loads. ---
	var packed := load(TEST_MAP)
	_check("test map %s loads" % TEST_MAP, packed != null)
	if packed == null:
		quit(1)
		return

	# --- Instantiate it into the live tree (drives camera_demo.gd::_ready, which
	#     builds the sandbox + drops the C++ MeridianTpsCamera). ---
	var scene: Node = packed.instantiate()
	_check("test map instantiates", scene != null)
	if scene == null:
		quit(1)
		return
	root.add_child(scene)


func _find_by_class(node: Node, cls: String) -> Node:
	if node.get_class() == cls:
		return node
	for child in node.get_children():
		var hit := _find_by_class(child, cls)
		if hit != null:
			return hit
	return null


func _process(_delta: float) -> bool:
	_frame += 1
	# Give the map a few frames to build + render on the real device before we
	# sample the backend and grab the screenshot.
	if _frame < 30 or _reported:
		return false
	_reported = true

	var adapter := RenderingServer.get_video_adapter_name()
	var api := RenderingServer.get_video_adapter_api_version()
	print("  active backend : %s (api %s)" % [adapter, api])

	# --- A real GPU device is up (empty name == the dummy/headless driver). ---
	_check("real GPU device present (adapter reported)", adapter != "")

	# --- The C++ GDExtension loaded and the map's camera instantiated. ---
	_check("MeridianTpsCamera (C++) present in map",
		_find_by_class(root, "MeridianTpsCamera") != null)

	# --- Screenshot evidence: prove the map actually rendered a frame. ---
	var img: Image = root.get_texture().get_image()
	var shot := "user://renderer_boot_%s.png" % api.replace(" ", "_").replace(".", "_")
	var wrote := img != null and img.save_png(shot) == OK
	_check("screenshot written (%s)" % shot, wrote)
	if wrote:
		print("  screenshot     : %s" % ProjectSettings.globalize_path(shot))

	print("meridian renderer-backend boot verify: %s" % ("PASS" if _fails == 0 else "FAIL"))
	quit(1 if _fails > 0 else 0)
	return true
