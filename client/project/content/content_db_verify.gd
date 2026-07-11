# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for MeridianContentDB (issue
# #477, client-assembler spec ② §3). NOT a shipped scene: a SceneTree script run
#   godot --headless --script res://content/content_db_verify.gd
# (same convention as scenes/charselect/char_select_verify.gd) so CI / a dev box
# can prove, with no render and no server, that the client reads mcc-built pack
# content correctly and that every miss returns its documented empty sentinel.
#
# It exercises the BINDING API against the staged core pack (res://meridian/core,
# the mcc emit-pck output): the ardent/male catalog, the rusty-pickaxe worn block,
# the russet dye color, asset res:// resolution — then the sentinel paths (unknown
# race/item/dye/asset) and the content-missing path (a bogus pack dir).
#
# Exits 0 on success, 1 on any failed assertion.

extends SceneTree

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian MeridianContentDB RUNTIME verify (#477)")

	# Load the staged core pack explicitly (the autoload loads it at boot too; loading
	# here makes the verify self-contained and independent of autoload ordering).
	# Untyped so method calls bind dynamically to the autoload's script (#477).
	var db = ContentDB
	var loaded: bool = db.load_from("res://meridian/core")
	_check("staged core pack loads (res://meridian/core)", loaded and db.is_loaded())

	_verify_catalog(db)
	_verify_worn(db)
	_verify_dye(db)
	_verify_model_path(db)
	_verify_sentinels(db)
	_verify_content_missing(db)

	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


# --- catalog(1, 0) — the ardent/male catalog ----------------------------------
func _verify_catalog(db) -> void:
	print(" catalog(1, 0) — ardent male (MeridianRoster id 1):")
	var cat := db.catalog(1, 0)
	_check("catalog is non-empty", not cat.is_empty())
	_check("catalog.body_model is set", String(cat.get("body_model", "")) == "core:art.char.ardent.male.base")
	_check("catalog.skeleton is set", String(cat.get("skeleton", "")) == "core:art.char.ardent.male.skeleton")
	var presets: Dictionary = cat.get("presets", {})
	_check("presets carry hair/face/skin lists",
		presets.has("hair") and presets.has("face") and presets.has("skin"))
	_check("hair presets are non-empty {id, model}",
		not presets["hair"].is_empty()
		and int(presets["hair"][0]["id"]) == 1
		and not String(presets["hair"][0]["model"]).is_empty())


# --- worn(rusty_pickaxe) — the socketed weapon --------------------------------
func _verify_worn(db) -> void:
	print(" worn(core:item.rusty_pickaxe):")
	var pick := db.numeric_id_for("core:item.rusty_pickaxe")
	_check("rusty_pickaxe resolves to a numeric id", pick != 0)
	var w := db.worn(pick)
	_check("worn is non-empty", not w.is_empty())
	var attach: Dictionary = w.get("attach", {})
	_check("worn.attach.socket == 'main_hand'", String(attach.get("socket", "")) == "main_hand")
	_check("worn.models carries the weapon model",
		not w.get("models", []).is_empty()
		and String(w["models"][0]["model"]) == "core:art.item.weapon.pickaxe_rusty")


# --- dye_color(russet) — the authored color -----------------------------------
func _verify_dye(db) -> void:
	print(" dye_color(core:dye.russet):")
	var russet := db.numeric_id_for("core:dye.russet")
	_check("russet resolves to a numeric id", russet != 0)
	var c := db.dye_color(russet)
	_check("dye_color == authored #8a4b2d", c.is_equal_approx(Color.html("8a4b2d")))


# --- model_path — asset res:// resolution -------------------------------------
func _verify_model_path(db) -> void:
	print(" model_path(id) — by content id AND by numeric id:")
	var by_id := db.model_path("core:art.char.ardent.male.base")
	_check("model_path(content id) returns a res:// path", by_id.begins_with("res://meridian/"))
	var numeric := db.numeric_id_for("core:art.char.ardent.male.base")
	_check("model_path(numeric id) matches model_path(content id)",
		numeric != 0 and db.model_path(numeric) == by_id)


# --- documented empty sentinels (spec §6 / contract ① §9) ---------------------
func _verify_sentinels(db) -> void:
	print(" empty sentinels (miss → documented default, never a crash):")
	_check("catalog(unknown race) → {}", db.catalog(2, 0).is_empty())        # Dolmen: no catalog
	_check("worn(unknown item) → {}", db.worn(999999999).is_empty())
	_check("dye_color(unknown) → Color(0,0,0,0)", db.dye_color(999999999) == Color(0, 0, 0, 0))
	_check("model_path(unknown id) → ''", db.model_path("core:art.nope") == "")
	_check("model_path(unknown numeric) → ''", db.model_path(999999999) == "")
	_check("numeric_id_for(unknown) → 0", db.numeric_id_for("core:nope.nope") == 0)


# --- content-missing path (a pack dir with no artifacts) ----------------------
func _verify_content_missing(db) -> void:
	print(" content-missing (bogus pack dir → not loaded, all sentinels):")
	var ok := db.load_from("res://meridian/does_not_exist")
	_check("load_from(bogus dir) returns false", not ok and not db.is_loaded())
	_check("catalog() → {} when unloaded", db.catalog(1, 0).is_empty())
	_check("dye_color() → sentinel when unloaded", db.dye_color(1) == Color(0, 0, 0, 0))
	# Restore the real pack so a caller after the verify sees loaded content.
	db.load_from("res://meridian/core")
