# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the THEME-DRIVEN pack mount
# (issue #791; chibi-theme design §4/§8). NOT a shipped scene: a SceneTree script
#   godot --headless --path client/project --script res://content/content_db_theme_verify.gd
# proves, with no render and no server, that:
#   1. resolve_pack_dir() applies the #791 precedence — default → res://meridian/core
#      (back-compat), MERIDIAN_REALM_THEME=<ns> → res://meridian/<ns>, an explicit
#      MERIDIAN_PACK_DIR wins over both, and an unsafe theme falls back to core.
#   2. With the chibi pack staged at res://meridian/chibi (C12/build stages it; this
#      verify stages it via check-golden's emit for local runs), the ContentDB
#      _init env path MOUNTS chibi (not core) when MERIDIAN_REALM_THEME=chibi.
#   3. The chibi mount's roster/appearance content — the DATA char-create's colored
#      preview + the assembler consume — is READABLE through the public ContentDB
#      API (the 6 chibi races' appearance ids resolve; the shared body + a race dye
#      resolve to res://meridian/chibi/... paths). (The char-create PICKER reading
#      the chibi roster names is C7's roster-from-pack work; this proves the mounted
#      DATA is present and queryable.)
#   4. MeridianPackMount.mount_and_verify(res://meridian/chibi) resolves the chibi
#      content identity — a 64-hex content_hash + content_version "chibi@<ver>" — the
#      hash the server advertises in the IF-2 HandshakeOk for the later fail-closed
#      content-hash tie (MeridianPackMount.set_expected_content_hash).
#   5. The default (no env) path still mounts core and the core catalog still reads
#      (nothing regressed for the baseline realm).
#
# Exits 0 on success, 1 on any failed assertion.

extends SceneTree

# Standalone --script mode never initializes autoloads, so the `ContentDB` autoload
# name does NOT exist here — preload the script and instantiate directly (same
# convention as content_db_verify.gd). The real app exercises this identical code as
# the autoload (content_db.gd loads in _init, tree-independent).
const ContentDbScript := preload("res://content/content_db.gd")

const CORE_DIR := "res://meridian/core"
const CHIBI_DIR := "res://meridian/chibi"

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian THEME-DRIVEN pack mount RUNTIME verify (#791)")

	_verify_resolution()
	_verify_env_mounts_chibi()
	_verify_chibi_roster_data()
	_verify_pack_mount_identity()
	_verify_core_backcompat()

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


# --- 1. resolve_pack_dir() / resolve_theme() precedence -----------------------
func _verify_resolution() -> void:
	print(" resolve_pack_dir() precedence (MERIDIAN_PACK_DIR > MERIDIAN_REALM_THEME > core):")
	# Snapshot + clear both env inputs so the checks are order-independent.
	var saved_theme := OS.get_environment(ContentDbScript.REALM_THEME_ENV)
	var saved_pack := OS.get_environment(ContentDbScript.PACK_DIR_ENV)
	OS.set_environment(ContentDbScript.REALM_THEME_ENV, "")
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, "")

	_check("no env → default res://meridian/core (back-compat)",
		ContentDbScript.resolve_pack_dir() == CORE_DIR)
	_check("DEFAULT_PACK_DIR is res://meridian/core", ContentDbScript.DEFAULT_PACK_DIR == CORE_DIR)

	OS.set_environment(ContentDbScript.REALM_THEME_ENV, "chibi")
	_check("MERIDIAN_REALM_THEME=chibi → res://meridian/chibi",
		ContentDbScript.resolve_pack_dir() == CHIBI_DIR)
	_check("resolve_theme() == 'chibi'", ContentDbScript.resolve_theme() == "chibi")

	# An explicit full pack dir override wins over the theme.
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, "res://meridian/core")
	_check("MERIDIAN_PACK_DIR wins over the theme",
		ContentDbScript.resolve_pack_dir() == CORE_DIR)
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, "")

	# Path-traversal / unsafe themes are rejected → fall back to core (never escapes
	# res://meridian/).
	for bad in ["../secret", "core/../..", "a/b", "Chibi", "chi bi"]:
		OS.set_environment(ContentDbScript.REALM_THEME_ENV, bad)
		_check("unsafe theme '%s' → falls back to core" % bad,
			ContentDbScript.resolve_pack_dir() == CORE_DIR)

	# Restore the caller's environment.
	OS.set_environment(ContentDbScript.REALM_THEME_ENV, saved_theme)
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, saved_pack)


# --- 2. env-driven _init mounts chibi (the full boot path) --------------------
func _verify_env_mounts_chibi() -> void:
	print(" ContentDB _init with MERIDIAN_REALM_THEME=chibi mounts the chibi pack:")
	var saved_theme := OS.get_environment(ContentDbScript.REALM_THEME_ENV)
	var saved_pack := OS.get_environment(ContentDbScript.PACK_DIR_ENV)
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, "")
	OS.set_environment(ContentDbScript.REALM_THEME_ENV, "chibi")

	# A fresh instance runs the real _init resolution (env → resolve_pack_dir → load).
	var db = ContentDbScript.new()
	_check("chibi pack is loaded via _init", db.is_loaded())
	_check("mounted pack_dir is res://meridian/chibi", db.pack_dir() == CHIBI_DIR)
	db.free()

	OS.set_environment(ContentDbScript.REALM_THEME_ENV, saved_theme)
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, saved_pack)


# --- 3. chibi roster/appearance data reads from the chibi mount ---------------
# The 6 chibi color races each ship an appearance catalog + baked body material +
# race dye — the DATA char-create's colored preview consumes. Proven readable
# through the public ContentDB API against the chibi mount (roster-name→id picker
# wiring is C7).
func _verify_chibi_roster_data() -> void:
	print(" chibi roster/appearance content reads from res://meridian/chibi:")
	var db = ContentDbScript.new()
	var loaded: bool = db.load_from(CHIBI_DIR)
	_check("chibi pack loads (res://meridian/chibi)", loaded and db.is_loaded())

	# All 6 color races × 2 sexes → 12 appearance catalogs whose ids resolve.
	var races := ["red", "green", "blue", "yellow", "gold", "silver"]
	var resolved := 0
	for r in races:
		for s in ["male", "female"]:
			var cid := "chibi:appearance.%s.%s" % [r, s]
			if db.numeric_id_for(cid) != 0:
				resolved += 1
	_check("all 12 chibi appearance catalogs resolve to numeric ids", resolved == 12)

	# The shared chibi body mesh resolves to a chibi-mount res:// path (the body the
	# colored preview instances).
	var body: String = db.model_path("chibi:art.chibi_pill_body")
	_check("chibi body model resolves under res://meridian/chibi/",
		body.begins_with("res://meridian/chibi/"))

	# A per-race dye resolves to its authored color (the recolor the preview applies).
	var blue_dye: int = db.numeric_id_for("chibi:dye.blue")
	_check("chibi blue dye resolves to a numeric id", blue_dye != 0)
	_check("chibi blue dye color is authored #3a9fe6",
		db.dye_color(blue_dye).is_equal_approx(Color.html("3a9fe6")))
	db.free()


# --- 4. MeridianPackMount content identity for the chibi mount ----------------
# Proves the chibi pack's content_hash + content_version resolve — the identity the
# server advertises in the IF-2 HandshakeOk (content_hash) for the later fail-closed
# content-hash tie. Requires the built GDExtension; skipped-with-warning if absent.
func _verify_pack_mount_identity() -> void:
	print(" MeridianPackMount.mount_and_verify(res://meridian/chibi) content identity:")
	if not ClassDB.class_exists("MeridianPackMount"):
		print("  [WARN] MeridianPackMount (GDExtension) not loaded — build the client")
		print("         (scripts/dev/build-client.sh) to run this check. SKIPPED.")
		return
	var mount = ClassDB.instantiate("MeridianPackMount")
	var res: Dictionary = mount.mount_and_verify(CHIBI_DIR)
	_check("chibi manifest verifies OK", bool(res.get("ok", false)))
	var chash := String(res.get("content_hash", ""))
	_check("content_hash resolves to 64 hex chars", chash.length() == 64)
	_check("content_version is 'chibi@<ver>'",
		String(res.get("content_version", "")).begins_with("chibi@"))
	# The identity is cached on the instance for the later IF-2 connect.
	_check("mount caches the chibi content identity",
		mount.is_mounted() and mount.get_content_version().begins_with("chibi@"))


# --- 5. default (no env) still mounts core — nothing regressed ----------------
func _verify_core_backcompat() -> void:
	print(" default (no theme) still mounts core + core catalog reads:")
	var saved_theme := OS.get_environment(ContentDbScript.REALM_THEME_ENV)
	var saved_pack := OS.get_environment(ContentDbScript.PACK_DIR_ENV)
	OS.set_environment(ContentDbScript.REALM_THEME_ENV, "")
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, "")

	var db = ContentDbScript.new()
	_check("default mount is res://meridian/core", db.pack_dir() == CORE_DIR)
	_check("core ardent/male catalog still reads",
		not db.catalog(1, 0).is_empty()
		and String(db.catalog(1, 0).get("body_model", "")) == "core:art.char.ardent.male.base")
	db.free()

	OS.set_environment(ContentDbScript.REALM_THEME_ENV, saved_theme)
	OS.set_environment(ContentDbScript.PACK_DIR_ENV, saved_pack)
