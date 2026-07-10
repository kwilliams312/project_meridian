# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — client appearance preset set for character create (issue #435,
# CHR-01, "appearance set").
#
# ⚠ M1 PLACEHOLDER — NOT the server appearance catalog. The appearance_catalog content
# set (contract ① §5.1, #461/#467) added the SCHEMA (schema/content/appearance.schema.yaml)
# and the dye content, but at the time of this change NO per-race/sex catalog files exist
# under content/<ns>/appearance/, and there is NO wire message or client content pack that
# exposes a catalog to the client. So the create screen offers this small documented
# placeholder set instead of a data-driven one. See the report/PR for the follow-up:
# expose the appearance_catalog to the client (a wire op or a client-loadable pack), then
# drive these pickers from it and delete this stub — the SAME report-don't-fake discipline
# used across epic #24 (#439/#453/#457).
#
# The preset ids MIRROR the wire Appearance record (world.fbs Appearance): hair/face/skin
# are 1-based uint8 ids, version is the record version (only v1 at M1). The server treats
# appearance as opaque-but-bounded (spec §9) — it CLAMPS out-of-range values, never rejects
# — so these ids are purely a client-side menu; picking any of them always creates.
# morphs (§2.5 crowd budget) ship at 0 for M1 and are not offered here.

extends RefCounted
class_name MeridianAppearance

## Appearance record version carried on the wire (world.fbs Appearance.version). Only v1
## exists at M1; bump when a new record layout lands.
const VERSION: int = 1

## Hair presets — id (1-based, stable) -> display name. Append-only (a persisted
## character.appearance.hair is one of these ids).
const HAIR: Array[Dictionary] = [
	{"id": 1, "name": "Cropped"},
	{"id": 2, "name": "Braided"},
	{"id": 3, "name": "Flowing"},
]

## Face presets — id -> display name.
const FACE: Array[Dictionary] = [
	{"id": 1, "name": "Youthful"},
	{"id": 2, "name": "Weathered"},
	{"id": 3, "name": "Stern"},
]

## Skin presets — id -> display name + a preview tint. `color` is a CLIENT-ONLY
## convenience so the create preview visibly reflects the pick; it is never sent (only
## the id rides the wire). Palettes proper live in the future appearance_catalog.
const SKIN: Array[Dictionary] = [
	{"id": 1, "name": "Fair", "color": Color(0.92, 0.80, 0.68)},
	{"id": 2, "name": "Tan", "color": Color(0.78, 0.60, 0.44)},
	{"id": 3, "name": "Deep", "color": Color(0.45, 0.32, 0.24)},
]

## Default create selection (first preset of each channel). Purely a UI convenience —
## the server accepts (clamps) any value.
const DEFAULT_HAIR_ID: int = 1
const DEFAULT_FACE_ID: int = 1
const DEFAULT_SKIN_ID: int = 1


## True iff `id` names a defined hair preset.
static func is_valid_hair(id: int) -> bool:
	return _has_id(HAIR, id)


## True iff `id` names a defined face preset.
static func is_valid_face(id: int) -> bool:
	return _has_id(FACE, id)


## True iff `id` names a defined skin preset.
static func is_valid_skin(id: int) -> bool:
	return _has_id(SKIN, id)


## Preview tint for a skin id (falls back to the first preset for an unknown id, so the
## preview never goes blank — mirrors the server clamp of an out-of-range appearance).
static func skin_color(id: int) -> Color:
	for s in SKIN:
		if int(s["id"]) == id:
			return s["color"]
	return SKIN[0]["color"]


## The default appearance record ({version, hair, face, skin}) — the look a create uses
## when the player leaves the pickers untouched.
static func default_appearance() -> Dictionary:
	return {
		"version": VERSION,
		"hair": DEFAULT_HAIR_ID,
		"face": DEFAULT_FACE_ID,
		"skin": DEFAULT_SKIN_ID,
	}


static func _has_id(presets: Array, id: int) -> bool:
	for p in presets:
		if int(p["id"]) == id:
			return true
	return false
