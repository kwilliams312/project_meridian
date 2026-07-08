# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — LOCAL character-list store (issue #110, CHR-01 / D-11).
#
# ⚠ THIS IS THE OFFLINE FALLBACK — an in-memory store, NOT the server roster.
#
# The REAL, server-authoritative character CRUD now ships (D-35 / #279): when
# char_select.gd has a live session it drives CharList/CharCreate/CharDelete +
# ENTER_WORLD over the net thread (MeridianNetThread) directly, and the roster is the
# server's (worldd → meridian-characters CRUD, #85). This store is used ONLY when
# there is NO session — a warm boot / standalone open / the headless verify test —
# so the char-select scene stays buildable + testable without a server. (The online
# path is verified on-device per the MoltenVK/Apple-Silicon caveat, #283.)
#
# The validation here MIRRORS the server's create_character rules
# (server/characters/src/characters.h → CharCreateStatus) so the OFFLINE behaviour
# matches the server's:
#   1. name non-empty and <= MAX_NAME_LEN (VARCHAR(32))    -> "invalid_name"
#   2. race id in the M0-frozen roster (MeridianRoster)     -> "invalid_race"
#   3. class id in the M0-frozen roster (MeridianRoster)    -> "invalid_class"
#   4. name unique, case-insensitive (uq_character_name)    -> "duplicate_name"
# The server additionally enforces a one-character-per-account cap at create
# (#329, CharCreateStatus.LIMIT_REACHED); this local fallback does not cap (see create()).

extends RefCounted
class_name CharacterStore

## Mirrors character.name VARCHAR(32) / characters.h kMaxNameLen. An over-long name
## is rejected here with a clear error instead of being silently truncated.
const MAX_NAME_LEN: int = 32

# In-memory character rows. Each is { id:int, name:String, race:int, class:int }.
# `id` is locally minted (the real server mints it); ordered by creation (mirrors
# list_characters ORDER BY id).
var _rows: Array[Dictionary] = []
var _next_id: int = 1


## Every character in this account, in creation order. Returns copies so callers
## cannot mutate the store's rows in place.
func list() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for r in _rows:
		out.append(r.duplicate())
	return out


## Number of characters currently stored.
func count() -> int:
	return _rows.size()


## Attempt to create a character. Returns a result Dictionary:
##   { "ok": true,  "row": {id,name,race,class} }              on success
##   { "ok": false, "error": <code>, "detail": <human text> }  on rejection
## `error` is one of: "invalid_name", "invalid_race", "invalid_class",
## "duplicate_name" — the same failure taxonomy as the server's create exceptions.
## Validation order matches create_character exactly.
##
## NOTE (#329): the server also enforces a one-character-per-account cap inside the
## create transaction (CharCreateStatus.LIMIT_REACHED). This LOCAL stub intentionally
## does NOT cap — it holds several characters so the list/delete UI stays testable
## before the server wiring lands. When the TODO(#286) transport wiring replaces
## these methods, map LIMIT_REACHED onto an "account_limit" error code; the char_select
## view already renders any create failure's `detail` gracefully ("Cannot create: …"),
## so a new reason needs no view change.
func create(name: String, race_id: int, class_id: int) -> Dictionary:
	var trimmed := name.strip_edges()
	# 1. Name: non-empty, within VARCHAR(32).
	if trimmed.is_empty():
		return _err("invalid_name", "Name must not be empty.")
	if trimmed.length() > MAX_NAME_LEN:
		return _err("invalid_name", "Name exceeds %d characters." % MAX_NAME_LEN)
	# 2. Race in the M0-frozen roster.
	if not MeridianRoster.is_valid_race(race_id):
		return _err("invalid_race", "Unknown race id %d." % race_id)
	# 3. Class in the M0-frozen roster.
	if not MeridianRoster.is_valid_class(class_id):
		return _err("invalid_class", "Unknown class id %d." % class_id)
	# 4. Name unique (case-insensitive, uq_character_name).
	var lname := trimmed.to_lower()
	for r in _rows:
		if String(r["name"]).to_lower() == lname:
			return _err("duplicate_name", "The name '%s' is already taken." % trimmed)

	var row := {"id": _next_id, "name": trimmed, "race": race_id, "class": class_id}
	_next_id += 1
	_rows.append(row)
	return {"ok": true, "row": row.duplicate()}


## Delete the character with `character_id`. Returns true if a row was removed.
## Mirrors delete_character's "no-op when not owned/absent" contract (here: absent).
func delete(character_id: int) -> bool:
	for i in range(_rows.size()):
		if int(_rows[i]["id"]) == character_id:
			_rows.remove_at(i)
			return true
	return false


func _err(code: String, detail: String) -> Dictionary:
	return {"ok": false, "error": code, "detail": detail}
