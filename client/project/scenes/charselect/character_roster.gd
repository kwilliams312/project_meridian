# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — client mirror of the M0-frozen playable roster (issue #110,
# CHR-01 stub / sync decision D-11).
#
# SINGLE CANONICAL CLIENT COPY of the M0 race/class enumeration. It is a hand-kept
# mirror of the server's authoritative source, server/characters/src/roster.h — the
# character-create stub the client offers MUST reflect the SAME ids/names so a
# create the player builds here validates identically on the server (create_character
# rejects any race/class not in this set; the row stores these numeric ids).
#
# ⚠ KEEP IN SYNC with server/characters/src/roster.h. Ids are 1-based and STABLE
# (a persisted character.race / character.class is one of these ids); 0 is reserved
# as "unset/invalid" so a zero-initialised field never accidentally validates. Ids
# are append-only: later milestones extend these lists, they never renumber. If the
# server roster changes, update this file in the same change.
#
# Provenance: these ids/names are an ORIGINAL clean-room placeholder set (deliberately
# generic archetypes — no Blizzard/WoW roster consulted, CONTRIBUTING.md). The Game
# Design Baseline fixes only COUNTS per milestone (M1: 1 race, 2 classes; M2: 2 races,
# 4 classes); the concrete names live in roster.h and are mirrored here.

extends RefCounted
class_name MeridianRoster

## Reserved "unset/invalid" id (rejected by every validator). Mirrors roster.h.
const INVALID_ID: int = 0

## M0-frozen playable races (roster.h Race enum). id -> display name, in id order.
## id 1 (Ardent) is the M1 "1 playable race"; 2..4 are frozen placeholders that
## later milestones turn on. Append-only.
const RACES: Array[Dictionary] = [
	{"id": 1, "name": "Ardent"},    # resilient folk of the central realm (M1 playable)
	{"id": 2, "name": "Dolmen"},    # mountain / stone folk
	{"id": 3, "name": "Sylvane"},   # forest folk
	{"id": 4, "name": "Emberkin"},  # fire-touched folk
]

## M0-frozen playable classes (roster.h Class enum). id -> display name, in id order.
## M1 requires 1 melee + 1 caster: kVanguard (melee) + kRuncaller (caster).
const CLASSES: Array[Dictionary] = [
	{"id": 1, "name": "Vanguard"},   # front-line melee (M1 melee class)
	{"id": 2, "name": "Runcaller"},  # arcane caster    (M1 caster class)
	{"id": 3, "name": "Warden"},     # ranged / hybrid
	{"id": 4, "name": "Mender"},     # healer
]

## Count of ids in each frozen enum (roster.h kRaceCount / kClassCount). Valid ids
## are the contiguous range [1, count].
const RACE_COUNT: int = 4
const CLASS_COUNT: int = 4

## Default create selection: the M1-playable pair (Ardent + Vanguard). Purely a UI
## convenience — the server accepts any valid id.
const DEFAULT_RACE_ID: int = 1
const DEFAULT_CLASS_ID: int = 1


## True iff `race_id` is a defined M0-frozen race id (rejects 0 and out-of-range).
## Mirrors roster.h is_valid_race.
static func is_valid_race(race_id: int) -> bool:
	return race_id >= 1 and race_id <= RACE_COUNT


## True iff `class_id` is a defined M0-frozen class id (rejects 0 and out-of-range).
## Mirrors roster.h is_valid_class.
static func is_valid_class(class_id: int) -> bool:
	return class_id >= 1 and class_id <= CLASS_COUNT


## Human-readable race name for an id ("" for an unknown id). Mirrors race_name().
static func race_name(race_id: int) -> String:
	for r in RACES:
		if int(r["id"]) == race_id:
			return String(r["name"])
	return ""


## Human-readable class name for an id ("" for an unknown id). Mirrors class_name().
static func class_name_for(class_id: int) -> String:
	for c in CLASSES:
		if int(c["id"]) == class_id:
			return String(c["name"])
	return ""
