# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for MeridianContentDB (issue
# #477, client-assembler spec ② §3). NOT a shipped scene: a SceneTree script run
#   godot --headless --path client/project --script res://content/content_db_verify.gd
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

# Standalone --script mode never initializes autoloads, so the `ContentDB`
# autoload name does NOT exist here — preload the script and instantiate directly.
# The real app exercises the same code as the autoload (content_db.gd loads in
# _init, tree-independent), so this verifies the identical load + lookup path.
const ContentDbScript := preload("res://content/content_db.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _initialize() -> void:
	print("meridian MeridianContentDB RUNTIME verify (#477)")

	# Instantiate the DB directly (no autoloads in --script mode; see const above)
	# and load the staged core pack explicitly so the verify is self-contained.
	# Untyped so method calls bind dynamically to the script (#477).
	var db = ContentDbScript.new()
	var loaded: bool = db.load_from("res://meridian/core")
	_check("staged core pack loads (res://meridian/core)", loaded and db.is_loaded())

	_verify_catalog(db)
	_verify_catalog_dolmen(db)
	_verify_worn(db)
	_verify_dye(db)
	_verify_model_path(db)
	_verify_sentinels(db)
	_verify_roster_fallback(db)
	_verify_theme_roster(db)
	_verify_content_missing(db)

	db.free()  # plain Node, never entered the tree
	print("\n%s" % ("ALL RUNTIME CHECKS PASS" if _fails == 0 else "%d RUNTIME FAILURE(S)" % _fails))
	quit(0 if _fails == 0 else 1)


# --- catalog(1, 0) — the ardent/male catalog ----------------------------------
func _verify_catalog(db) -> void:
	print(" catalog(1, 0) — ardent male (MeridianRoster id 1):")
	# `db` is untyped (dynamic binding to the autoload), so its calls return Variant
	# — every local below is explicitly typed (`:=` cannot infer from Variant).
	var cat: Dictionary = db.catalog(1, 0)
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


# --- catalog(2, 0) — the dolmen/male catalog (Race #2 D3) ---------------------
# The second race's catalog resolves to the DOLMEN body + skeleton (D1/D2), NOT
# the Ardent ones — the ContentDB half of the D3 "catalog(2,0) → Dolmen body"
# proof. Presets reuse the Ardent asset ids (no-key D3), so the preset SHAPE
# matches ardent while the body/skeleton are dolmen-specific.
func _verify_catalog_dolmen(db) -> void:
	print(" catalog(2, 0) — dolmen male (MeridianRoster id 2, Race #2 D3):")
	var cat: Dictionary = db.catalog(2, 0)
	_check("dolmen catalog is non-empty", not cat.is_empty())
	_check("catalog.body_model is the DOLMEN body (not ardent)",
		String(cat.get("body_model", "")) == "core:art.char.dolmen.male.base")
	_check("catalog.skeleton is the DOLMEN skeleton (not ardent)",
		String(cat.get("skeleton", "")) == "core:art.char.dolmen.male.skeleton")
	# The dolmen body differs from the ardent body — the model-per-race payoff.
	_check("dolmen body_model differs from the ardent catalog's",
		String(cat.get("body_model", "")) != String(db.catalog(1, 0).get("body_model", "")))
	# The dolmen body id resolves to a staged res:// resource the assembler can load.
	_check("dolmen body_model resolves to a staged res:// path",
		db.model_path(String(cat.get("body_model", ""))).begins_with("res://meridian/"))
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
	var pick: int = db.numeric_id_for("core:item.rusty_pickaxe")
	_check("rusty_pickaxe resolves to a numeric id", pick != 0)
	var w: Dictionary = db.worn(pick)
	_check("worn is non-empty", not w.is_empty())
	var attach: Dictionary = w.get("attach", {})
	_check("worn.attach.socket == 'main_hand'", String(attach.get("socket", "")) == "main_hand")
	_check("worn.models carries the weapon model",
		not w.get("models", []).is_empty()
		and String(w["models"][0]["model"]) == "core:art.item.weapon.pickaxe_rusty")
	# The full worn shape is surfaced verbatim — race_overrides present (empty {}
	# for M1 content; the non-empty round-trip is covered by the mcc unit fixture).
	_check("worn surfaces race_overrides", w.has("race_overrides"))


# --- dye_color(russet) — the authored color -----------------------------------
func _verify_dye(db) -> void:
	print(" dye_color(core:dye.russet):")
	var russet: int = db.numeric_id_for("core:dye.russet")
	_check("russet resolves to a numeric id", russet != 0)
	var c: Color = db.dye_color(russet)
	_check("dye_color == authored #8a4b2d", c.is_equal_approx(Color.html("8a4b2d")))


# --- model_path — asset res:// resolution -------------------------------------
func _verify_model_path(db) -> void:
	print(" model_path(id) — by content id AND by numeric id:")
	var by_id: String = db.model_path("core:art.char.ardent.male.base")
	_check("model_path(content id) returns a res:// path", by_id.begins_with("res://meridian/"))
	var numeric: int = db.numeric_id_for("core:art.char.ardent.male.base")
	_check("model_path(numeric id) matches model_path(content id)",
		numeric != 0 and db.model_path(numeric) == by_id)


# --- documented empty sentinels (spec §6 / contract ① §9) ---------------------
func _verify_sentinels(db) -> void:
	print(" empty sentinels (miss → documented default, never a crash):")
	_check("catalog(unknown race) → {}", db.catalog(3, 0).is_empty())        # Sylvane (id 3): no catalog yet
	_check("worn(unknown item) → {}", db.worn(999999999).is_empty())
	_check("dye_color(unknown) → Color(0,0,0,0)", db.dye_color(999999999) == Color(0, 0, 0, 0))
	_check("model_path(unknown id) → ''", db.model_path("core:art.nope") == "")
	_check("model_path(unknown numeric) → ''", db.model_path(999999999) == "")
	_check("numeric_id_for(unknown) → 0", db.numeric_id_for("core:nope.nope") == 0)


# --- roster: compiled fallback for a pack that ships none (core, #760) --------
# The staged core pack declares NO `theme` block, so emit-pck omits the race/class roster
# arrays — races()/classes() are empty and the effective roster falls back to the compiled
# MeridianRoster (design §8/R3). This is the seam that keeps core char-create unchanged.
func _verify_roster_fallback(db) -> void:
	print(" roster (core ships none → compiled MeridianRoster fallback):")
	db.load_from("res://meridian/core")
	_check("core pack ships no race/class roster (races()/classes() empty)",
		db.races().is_empty() and db.classes().is_empty())
	_check("effective_races falls back to the compiled roster",
		db.effective_races().size() == MeridianRoster.RACES.size())
	_check("race_display_name falls back to the compiled name",
		db.race_display_name(1) == MeridianRoster.race_name(1))
	_check("is_valid_race falls back to the compiled range",
		db.is_valid_race(1) and not db.is_valid_race(99))


# --- roster: a THEME pack drives the pickers (chibi-shape, #760) ---------------
# A pack that ships a race/class roster (a theme pack) makes races()/classes() the source
# of truth: the effective roster IS the pack's, the catalog key resolves by the pack's race
# name, and the appearance carries body_material — proving content_db's pack-driven read.
func _verify_theme_roster(db) -> void:
	print(" roster (theme pack ships a roster → pack-driven):")
	var dir := "user://content_db_theme_fixture"
	DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(dir + "/pack.data.json", FileAccess.WRITE)
	f.store_string(JSON.stringify({
		"schema": "meridian/pack-data@1", "namespace": "chibi",
		"appearance": [{
			"id": "chibi:appearance.gold.male", "numeric_id": 2001,
			"race": "gold", "sex": "male",
			"skeleton": "chibi:art.body", "body_model": "chibi:art.body",
			"body_material": {"albedo": "chibi:art.recolor", "dye_mask": "chibi:art.mask",
				"metallic": 1.0, "roughness": 0.3,
				"dyes": [{"channel": "primary", "dye": "chibi:dye.gold"}]},
			"presets": {"hair": [], "face": [], "skin": []},
		}],
		"class": [{"id": "chibi:class.warrior", "numeric_id": 3001, "roster_id": 1, "name": "Warrior"},
			{"id": "chibi:class.mage", "numeric_id": 3002, "roster_id": 2, "name": "Mage"}],
		"dye": [], "item": [],
		"race": [{"id": "chibi:race.red", "numeric_id": 1001, "roster_id": 1, "name": "Red"},
			{"id": "chibi:race.gold", "numeric_id": 1005, "roster_id": 5, "name": "Gold"}],
	}, "  "))
	f.close()
	db.load_from(dir)

	_check("races() returns the pack roster (ordered by roster_id)",
		db.races().size() == 2 and int(db.races()[0]["id"]) == 1 and int(db.races()[1]["id"]) == 5
		and String(db.races()[1]["name"]) == "Gold")
	_check("classes() returns the pack roster", db.classes().size() == 2)
	_check("effective_races IS the pack roster (not the compiled one)",
		db.effective_races().size() == 2)
	_check("race_display_name reads the pack name (roster_id 5 → Gold)",
		db.race_display_name(5) == "Gold")
	_check("is_valid_race honours the pack roster (5 valid, 2 not offered)",
		db.is_valid_race(5) and not db.is_valid_race(2))
	# The catalog key resolves by the pack's race NAME (roster_id 5 → 'gold'), and the
	# appearance carries the additive body_material recolor.
	var cat: Dictionary = db.catalog(5, 0)
	_check("catalog(5,0) resolves via the pack race name ('gold|male')", not cat.is_empty())
	_check("catalog carries the additive body_material",
		cat.has("body_material") and String(cat["body_material"]["albedo"]) == "chibi:art.recolor"
		and float(cat["body_material"]["metallic"]) == 1.0)


# --- content-missing path (a pack dir with no artifacts) ----------------------
func _verify_content_missing(db) -> void:
	print(" content-missing (bogus pack dir → not loaded, all sentinels):")
	var ok: bool = db.load_from("res://meridian/does_not_exist")
	_check("load_from(bogus dir) returns false", not ok and not db.is_loaded())
	_check("catalog() → {} when unloaded", db.catalog(1, 0).is_empty())
	_check("dye_color() → sentinel when unloaded", db.dye_color(1) == Color(0, 0, 0, 0))
	# Restore the real pack so a caller after the verify sees loaded content.
	db.load_from("res://meridian/core")
