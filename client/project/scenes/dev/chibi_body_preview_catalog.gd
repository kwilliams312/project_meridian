# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — DEV-ONLY throwaway catalog for the chibi_pill_body preview
# (story #747, epic #722). Shared by scenes/dev/chibi_body_preview.gd (the visible
# preview scene) and chibi_body_preview_verify.gd (the headless proof) so both
# assemble the chibi body through the EXACT SAME in-memory catalog.
#
# ⛔ NOTHING HERE IS PERSISTED OR SHIPPED. The "races = colors" appearance model is
# not designed yet, so this deliberately does NOT add a roster/race id, a shipped
# appearance catalog, or any content id — burning an append-only id for an E2E is
# forbidden (story #747). Instead it writes a THROWAWAY pack to `user://` (Godot's
# per-user scratch dir — never committed, never touches roster.lock/content) whose
# single appearance entry points a DEV race number at the already-landed
# `core:art.chibi_pill_body` asset, then loads it via the real ContentDB.load_from
# public API. The body's model bytes come from the real staged pack
# (client/project/meridian/core/art/chibi_pill_body.glb, staged by check-golden.sh
# STAGED_ART) through the honest engine load path — same as production.
#
# WHY A DEV RACE NUMBER (not 1..4). MeridianRoster freezes race ids 1..4; catalog()
# keys by MeridianRoster.race_name(race).to_lower() | sex. An UNKNOWN race id yields
# race_name() == "" (see character_roster.gd), so DEV_RACE keys the catalog under
# "|male" — an ephemeral slot that can never collide with a real roster race. The
# appearance entry's `race` field is written from race_name(DEV_RACE) so the stored
# key and the catalog() lookup key are guaranteed identical.
#
# WHY PRESETS ALIAS THE BODY MODEL. AssembledCharacter._resolve_preset emits an
# assembly_failed for a hair/face/skin id it cannot find — so EMPTY preset arrays
# would fire three failures. Aliasing preset id 1 to the body id (the blockout
# catalog's convention, assembled_character.gd:131) makes the resolve succeed with
# ZERO failures AND skips the hair mount (hair_model == body_id ⇒ not mounted), so a
# bare body assembles clean: is_assembled() == true, zero assembly_failed.

extends RefCounted

## The landed SP5 chibi base body asset (content id + its IF-9 numeric id, #745/#746).
const BODY_ID: String = "core:art.chibi_pill_body"
const BODY_NUMERIC: int = 184
## The DECLARED imported-resource path (M0 is declarative; the .glb sibling next to
## it carries the real bytes — see AssembledCharacter._load_model_scene).
const BODY_RESOURCE: String = "res://meridian/core/art/chibi_pill_body.scn"

## A THROWAWAY race number — NOT a MeridianRoster id (those are frozen 1..4, append
## only). race_name() returns "" for it, so it can never collide with a real race.
const DEV_RACE: int = 900
## M1 sex convention (0 = male) — matches ContentDB._sex_name(0) == "male".
const DEV_SEX: int = 0

## Godot per-user scratch dir — throwaway, never committed, never persisted to content.
const FIXTURE_DIR: String = "user://chibi_body_preview"


## Write the throwaway pack + load it into `db` (a MeridianContentDB). Returns the
## DEV race number to pass to AssembledCharacter.assemble(). After this call
## db.catalog(DEV_RACE, DEV_SEX) resolves the chibi body with body-aliased presets.
static func install(db) -> int:
	DirAccess.make_dir_recursive_absolute(FIXTURE_DIR)

	# pack.contents.jsonl — one {id, numeric_id, resource, hash} object per line.
	# The single chibi entry is all model_path(BODY_ID) needs to resolve the .glb.
	var contents: PackedStringArray = [
		JSON.stringify({
			"id": BODY_ID, "numeric_id": BODY_NUMERIC,
			"resource": BODY_RESOURCE, "hash": "",
		}),
	]
	var jsonl := FileAccess.open(FIXTURE_DIR + "/pack.contents.jsonl", FileAccess.WRITE)
	jsonl.store_string("\n".join(contents) + "\n")
	jsonl.close()

	# Presets alias the body model (id 1) so a bare-body assemble fires ZERO
	# assembly_failed (see header). face/skin are merely recorded; hair is skipped
	# because its model == the body id.
	var preset_list: Array = [{"id": 1, "model": BODY_ID}]
	# The race field MUST equal race_name(DEV_RACE) so the stored catalog key matches
	# the key catalog(DEV_RACE, DEV_SEX) computes (both "" | "male" here).
	var race_name: String = MeridianRoster.race_name(DEV_RACE)
	var data: Dictionary = {
		"schema": "meridian/pack-data@1",
		"namespace": "core",
		"appearance": [{
			"id": "core:appearance.dev.chibi_body_preview", "numeric_id": 900001,
			"race": race_name, "sex": "male",
			"skeleton": "",
			"body_model": BODY_ID,
			"presets": {"hair": preset_list, "face": preset_list, "skin": preset_list},
		}],
		"dye": [],
		"item": [],
	}
	var json := FileAccess.open(FIXTURE_DIR + "/pack.data.json", FileAccess.WRITE)
	json.store_string(JSON.stringify(data, "  "))
	json.close()

	db.load_from(FIXTURE_DIR)
	return DEV_RACE
