# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless-capable runtime verification for the login screen
# sizing/layout (issue #638, part of #637). NOT a shipped scene: this is a
# SceneTree script run windowed under the pinned Godot 4.7 editor:
#
#   godot --rendering-driver metal --path client/project \
#         --script res://scenes/login/login_screen_verify.gd --quit-after 120
#
# It proves, on a real device (the login scene loads the MeridianLogin GDExtension,
# so a dummy/headless device would abort — see #283), that after #645 (was #638):
#   * login_screen.tscn instantiates and every control the flow wires is present
#     (Host / Port / Account / Password / Login button / Status) with its defaults,
#   * the scene root carries a scoped Theme raising default_font_size to 26 (up from
#     Godot's default 16, and from #638's 20) so the screen reads comfortably in the
#     #630 1728×972 window, with an even larger per-control Title font,
#   * the form is a centered, substantially-sized card (a fixed size, NOT edge-to-edge),
#   * under the project `canvas_items`/`expand` stretch the canvas scale tracks the
#     window size on resize (base 1.0, larger >1.0, smaller <1.0) — same behavior #630
#     established — and a screenshot is written as evidence.
# Exits 0 on success, 1 on any failed assertion. This is the automatable counterpart
# to booting login_screen.tscn interactively (run-client.sh --scene …/login_screen.tscn).

extends SceneTree

const LOGIN_SCENE := "res://scenes/login/login_screen.tscn"
const BASE := Vector2(1728, 972)   # #630 base viewport (project.godot [display]).
const EXPECTED_FONT_SIZE := 26     # #645 scoped Theme default_font_size (was 20 in #638).
const EXPECTED_TITLE_FONT_SIZE := 42  # #645 per-control Title font override (> default).

var _fails := 0
var _frame := 0
var _reported := false
var _root_control: Control = null


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian login-screen sizing verify (#645)")

	# --- The scene resource loads + instantiates (drives login_screen.gd::_ready,
	#     which constructs LoginFlow via the C++ GDExtension). ---
	var packed := load(LOGIN_SCENE)
	_check("%s loads" % LOGIN_SCENE, packed != null)
	if packed == null:
		quit(1)
		return
	var scene: Control = packed.instantiate()
	_check("login scene instantiates", scene != null)
	if scene == null:
		quit(1)
		return
	_root_control = scene
	get_root().add_child(scene)

	# --- Every control the login flow wires is present (@onready %lookups). ---
	var host: LineEdit = scene.get_node_or_null("%HostEdit")
	var port: LineEdit = scene.get_node_or_null("%PortEdit")
	var account: LineEdit = scene.get_node_or_null("%AccountEdit")
	var password: LineEdit = scene.get_node_or_null("%PasswordEdit")
	var login_button: Button = scene.get_node_or_null("%LoginButton")
	var status: Label = scene.get_node_or_null("%StatusLabel")
	_check("HostEdit present, default 127.0.0.1",
		host != null and host.text == "127.0.0.1")
	_check("PortEdit present, default 7100",
		port != null and port.text == "7100")
	_check("AccountEdit present", account != null)
	_check("PasswordEdit present + secret", password != null and password.secret)
	_check("LoginButton present", login_button != null)
	_check("StatusLabel present", status != null)

	# --- The scoped Theme raises the font size for this scene only (#645). ---
	var theme := scene.theme
	_check("scene root carries a scoped Theme", theme != null)
	_check("Theme default_font_size == %d (up from Godot's 16, and #638's 20)" % EXPECTED_FONT_SIZE,
		theme != null and theme.default_font_size == EXPECTED_FONT_SIZE)
	# The larger size actually cascades to a control in this scene. A control with no
	# explicit per-type font_size override (as here) draws at the theme's DEFAULT font
	# size, so get_theme_default_font_size() is what the Button/LineEdits render at.
	if login_button != null:
		_check("Login button cascades the enlarged default font size (>=%d)" % EXPECTED_FONT_SIZE,
			login_button.get_theme_default_font_size() >= EXPECTED_FONT_SIZE)
	# #645: the Title carries a per-control font override larger than the field default,
	# so the heading reads bigger than the form rows (get_theme_font_size("font_size")
	# resolves the Label's own override, not the theme default).
	var title: Label = scene.get_node_or_null("VBox/Title")
	_check("Title present", title != null)
	if title != null:
		var title_size := title.get_theme_font_size("font_size")
		_check("Title font (%d) == the #645 heading size %d" % [title_size, EXPECTED_TITLE_FONT_SIZE],
			title_size == EXPECTED_TITLE_FONT_SIZE)
		_check("Title font is larger than the field default (%d > %d)" % [title_size, EXPECTED_FONT_SIZE],
			title_size > EXPECTED_FONT_SIZE)

	# --- The form is a centered, substantially-sized card — NOT stretched edge-to-edge.
	#     #645 grew it to 900×620 (~52% wide, ~64% tall of the 1728×972 base). The bounds
	#     below assert it is BOTH large enough to fill a comfortable share of the window AND
	#     still bounded well inside it (a clear margin on every side), so it can neither
	#     regress to the tiny #638 card nor sprawl edge-to-edge. ---
	var card: Control = scene.get_node_or_null("%VBox")
	_check("login card (VBox) present", card != null)
	if card != null:
		var w := card.offset_right - card.offset_left
		var h := card.offset_bottom - card.offset_top
		_check("card is substantially large (>=800×520, comfortably fills the window): %dx%d" % [int(w), int(h)],
			w >= 800.0 and h >= 520.0)
		_check("card stays bounded inside the window with a clear margin (w<=70%%, h<=80%% of base): %dx%d" % [int(w), int(h)],
			w <= BASE.x * 0.70 and h <= BASE.y * 0.80)
		_check("card centered on the viewport (anchor 0.5)",
			is_equal_approx(card.anchor_left, 0.5) and is_equal_approx(card.anchor_top, 0.5))


func _canvas_scale() -> Vector2:
	# Under `canvas_items`/`expand` the 2D canvas is rendered at BASE then scaled to
	# the window, so the effective UI scale is window_size / base_size per axis.
	var win := Vector2(DisplayServer.window_get_size())
	return Vector2(win.x / BASE.x, win.y / BASE.y)


func _process(_delta: float) -> bool:
	_frame += 1
	# Give the scene a few frames to build + render on the real device first.
	if _frame < 30 or _reported:
		return false
	_reported = true

	var adapter := RenderingServer.get_video_adapter_name()
	print("  active backend : %s" % adapter)
	_check("real GPU device present (adapter reported)", adapter != "")

	# --- Screenshot the freshly-rendered login screen at the base window size FIRST,
	#     before any resizing, so the capture is of a clean, fully-drawn frame. ---
	var win := get_root()
	var img: Image = win.get_texture().get_image()
	var shot := "user://login_screen_645.png"
	var wrote := img != null and img.save_png(shot) == OK
	_check("screenshot written (%s)" % shot, wrote)
	if wrote:
		print("  screenshot     : %s" % ProjectSettings.globalize_path(shot))

	# --- Canvas scale tracks the window on resize (the #630 behavior). ---
	var base_scale := _canvas_scale()
	print("  window %dx%d → canvas scale ~%.3f" % [
		DisplayServer.window_get_size().x, DisplayServer.window_get_size().y, base_scale.x])
	_check("canvas scale ~1.0 at the base window size",
		is_equal_approx(snappedf(base_scale.x, 0.01), 1.0))

	# Grow the window: scale must grow with it.
	DisplayServer.window_set_size(Vector2i(2400, 1350))
	var up_scale := _canvas_scale()
	print("  window 2400x1350 → canvas scale ~%.3f" % up_scale.x)
	_check("canvas scale grows when the window grows (>1.0)", up_scale.x > 1.05)

	# Shrink the window: scale must shrink with it.
	DisplayServer.window_set_size(Vector2i(1000, 640))
	var down_scale := _canvas_scale()
	print("  window 1000x640 → canvas scale ~%.3f" % down_scale.x)
	_check("canvas scale shrinks when the window shrinks (<1.0)", down_scale.x < 0.95)

	# Restore the base size for a clean exit.
	DisplayServer.window_set_size(Vector2i(int(BASE.x), int(BASE.y)))

	print("meridian login-screen sizing verify: %s" % ("PASS" if _fails == 0 else "FAIL"))
	quit(1 if _fails > 0 else 0)
	return true
