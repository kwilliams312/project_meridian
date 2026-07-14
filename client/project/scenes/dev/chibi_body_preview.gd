# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — DEV-ONLY chibi_pill_body preview scene (story #747, epic #722).
#
# Opens a real client window showing the landed `core:art.chibi_pill_body` base
# mesh, rendered + rigged, through the REAL engine model-load path
# (AssembledCharacter → ContentDB.model_path → staged .glb via GLTFDocument) so a
# human can visually E2E it (this gates PR #746). Bare body only — no gear, no
# color/race, no login/world flow.
#
# ⛔ NOT wired into the shipped menu. It is launched explicitly, e.g.:
#     scripts/dev/run-client.sh --scene=res://scenes/dev/chibi_body_preview.tscn
# The catalog is a THROWAWAY user:// pack for a dev race number — nothing is
# persisted or shipped (see chibi_body_preview_catalog.gd for the full rationale).
#
# On boot it asserts the body actually assembled (is_assembled() == true, NOT the
# capsule fallback) with ZERO assembly_failed and prints a PASS/FAIL banner to the
# console; the same assertions run headlessly in chibi_body_preview_verify.gd.

extends Node3D

# Preloaded BY PATH (never the bare class name) — a headless --script run has no
# global class cache, and preload is immune (same trap the assembler guards, see
# assembled_character.gd:52).
const AssembledCharacterScript := preload("res://characters/assembled_character.gd")
const ChibiPreviewCatalog := preload("res://scenes/dev/chibi_body_preview_catalog.gd")

# Framed for the compact chibi pill (feet at y=0, head ~1.7). Slightly wider FOV +
# closer dolly than the roster paperdoll so the stubby body fills the window.
const _CAM_FOV: float = 42.0
const _CAM_POS: Vector3 = Vector3(0.0, 0.95, 3.2)
# Slow turntable so the human can inspect every side without input (radians/sec).
const _SPIN_RATE: float = 0.6

var _body: Node3D = null
var _failures: Array = []


func _ready() -> void:
	# A neutral lit environment so the unlit sides of the body still read (a bare
	# DirectionalLight would leave the back of the turntable pure black).
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.10, 0.11, 0.13)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.36, 0.40)
	env.ambient_light_energy = 1.0
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var key_light := DirectionalLight3D.new()
	key_light.rotation = Vector3(deg_to_rad(-45.0), deg_to_rad(35.0), 0.0)
	add_child(key_light)

	var cam := Camera3D.new()
	cam.position = _CAM_POS
	cam.fov = _CAM_FOV
	cam.current = true
	add_child(cam)

	# Assemble the chibi body via the throwaway dev catalog + the real load path.
	var db = MeridianContentDB.instance()
	var dev_race: int = ChibiPreviewCatalog.install(db)

	var ac = AssembledCharacterScript.new()
	ac.name = "ChibiBody"
	ac.assembly_failed.connect(_on_assembly_failed)
	add_child(ac)
	var ok: bool = ac.assemble(dev_race, ChibiPreviewCatalog.DEV_SEX, {}, [])
	_body = ac

	_report(ok, ac)


func _process(delta: float) -> void:
	# Turntable the assembled body so all sides are visible hands-free.
	if _body != null and is_instance_valid(_body):
		_body.rotate_y(_SPIN_RATE * delta)


func _on_assembly_failed(reason: String) -> void:
	_failures.append(reason)


# Print a PASS/FAIL banner mirroring the headless verify's assertions, so a human
# eyeballing the window also gets a console verdict.
func _report(ok: bool, ac) -> void:
	var assembled: bool = ok and ac.is_assembled()
	var skel: Skeleton3D = ac.body_skeleton() if assembled else null
	print("meridian chibi_pill_body DEV preview (#747)")
	print("  body id           : %s" % ChibiPreviewCatalog.BODY_ID)
	print("  assemble() ok     : %s" % ok)
	print("  is_assembled()    : %s (want true — NOT the capsule fallback)" % ac.is_assembled())
	print("  body skeleton     : %s bones" % (skel.get_bone_count() if skel != null else 0))
	print("  assembly_failed   : %d (want 0) %s" % [_failures.size(), _failures])
	var pass_all: bool = assembled and _failures.is_empty()
	print("  %s" % ("PASS — chibi body assembled + rigged, zero failures" if pass_all
		else "FAIL — see the fields above"))
	if not pass_all:
		push_error("chibi_body_preview: assembly did not pass (is_assembled=%s, failures=%s)"
			% [ac.is_assembled(), _failures])
