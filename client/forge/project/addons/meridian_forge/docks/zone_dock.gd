@tool
extends VBoxContainer
## ZoneDock — the M0 skeleton's ONE dock panel (issue #134).
##
## This is the Forge editor dock (Tools SAD §5.1 `docks/zone_dock`). Its skeleton
## job is to prove the plugin↔GDExtension bridge: it instantiates the forge_core
## `ForgeCore` class and renders its version() string + the ITerrainBackend
## region-alignment seam (§5.2 op 3) live. A correct read here in the running
## editor is the end-to-end proof that the EditorPlugin loaded the native library
## and called across the boundary. The real zone-authoring UI lands in M1.

const _CORE_CLASS := "ForgeCore"

var _status: RichTextLabel = null


func _ready() -> void:
	custom_minimum_size = Vector2(240, 160)
	_build_ui()
	_refresh()


func _build_ui() -> void:
	var title := Label.new()
	title.text = "Meridian Forge"
	title.add_theme_font_size_override("font_size", 16)
	add_child(title)

	var sub := Label.new()
	sub.text = "Zone dock — M0 skeleton (#134)"
	sub.modulate = Color(1, 1, 1, 0.6)
	add_child(sub)

	add_child(HSeparator.new())

	_status = RichTextLabel.new()
	_status.bbcode_enabled = true
	_status.fit_content = true
	_status.selection_enabled = true
	_status.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_status)

	var refresh := Button.new()
	refresh.text = "Call forge_core"
	refresh.pressed.connect(_refresh)
	add_child(refresh)


func _refresh() -> void:
	if _status == null:
		return

	# Guard: report honestly if the native library is not loaded rather than
	# throwing — the dock stays usable even when the binary is missing.
	if not ClassDB.class_exists(_CORE_CLASS):
		_status.text = "[color=#ff6666]forge_core NOT loaded[/color]\n" \
			+ "Build it: cmake -B build -DGODOTCPP_TARGET=editor && cmake --build build -j"
		return

	var core: Object = ClassDB.instantiate(_CORE_CLASS)
	if core == null:
		_status.text = "[color=#ff6666]ForgeCore.instantiate() returned null[/color]"
		return

	var version: String = core.version()
	var info: Dictionary = core.terrain_backend_info()

	var lines := PackedStringArray()
	lines.append("[b]bridge:[/b] [color=#33dd77]forge_core loaded[/color]")
	lines.append("[b]version:[/b] " + version)
	lines.append("")
	lines.append("[b]ITerrainBackend seam[/b] (SAD §5.2 op 3)")
	lines.append("  backend: " + str(info.get("backend", "?")))
	lines.append("  region_size_m: " + str(info.get("region_size_m", "?")))
	lines.append("  chunk_size_m: " + str(info.get("chunk_size_m", "?")))
	lines.append("  heightfield_side: " + str(info.get("heightfield_side", "?")))
	var aligns: bool = bool(info.get("aligns", false))
	var aligns_col := "#33dd77" if aligns else "#ff6666"
	lines.append("  aligns 128 m grid: [color=" + aligns_col + "]" + str(aligns) + "[/color]")
	_status.text = "\n".join(lines)
