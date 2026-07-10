# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the GREYBOX known-ability set, now a HEADLESS-VERIFY FIXTURE only
# (CMB-01/UI-01, #432/#472).
#
# ⛔ NOT the runtime seed anymore. The former wire-contract gap (no S→C spellbook push) is
# CLOSED: worldd pushes the character's REAL KNOWN_ABILITIES (0x3005, #457) at ENTER_WORLD
# and re-pushes after a growing TRAINER_LEARN, and the world scene routes it to
# MeridianEventBus.publish_known_abilities(). This fixture survives ONLY so hud/combat_verify.gd
# can drive the action bar / GCD / cast-bar with NO server — it mirrors the wire ability
# shape (including the #457 cast_ms / triggers_gcd metadata) so the verify exercises the same
# prediction paths as the real set.
#
# IDs ARE REAL WHERE THEY EXIST. worldd validates CAST_REQUEST.ability_id against its loaded
# AbilityStore (the mcc-compiled `content/core/idmap.lock`). Slots 1-2 carry the two abilities
# the store actually loads (minor_healing = 1, pickaxe_slam = 2); slots 3+ carry placeholder
# ids that do NOT resolve, so worldd cleanly rejects them (CAST_FAILED / UNKNOWN_ABILITY) —
# demonstrating the D-10 optimistic-GCD ROLLBACK in the headless verify.
extends RefCounted
class_name MeridianAbilitySet


# The greybox action-bar slot list, in hotkey order (1..N). Each entry mirrors the canonical
# ability Dictionary publish_known_abilities() produces + the action bar renders:
#   { ability_id:int, name:String, icon_id:int, hotkey:int,
#     cast_ms:int, triggers_gcd:bool, resource_type:int, resource_cost:int, range_m:float }
# `icon_id` is the placeholder art/ID hook — the action bar derives a stable placeholder
# swatch from it. The cast_ms / triggers_gcd fields let the verify exercise the #456 fix
# (an off-GCD ability starts no GCD; a cast_ms>0 ability drives the cast bar).
static func greybox_abilities() -> Array:
	return [
		# --- Real AbilityStore ids (idmap.lock) — resolve to CAST_START + CAST_RESULT ---
		# Pickaxe Slam: an instant melee strike that DOES trigger the GCD (range 5).
		{"ability_id": 2, "name": "Pickaxe Slam", "icon_id": 2, "hotkey": 1,
			"cast_ms": 0, "triggers_gcd": true, "resource_type": 0, "resource_cost": 0, "range_m": 5.0},
		# Minor Healing: a free self-heal flagged OFF the GCD — exercises the #456 fix
		# (a triggers_gcd:false ability must NOT start an optimistic GCD).
		{"ability_id": 1, "name": "Minor Healing", "icon_id": 1, "hotkey": 2,
			"cast_ms": 0, "triggers_gcd": false, "resource_type": 0, "resource_cost": 0, "range_m": 0.0},
		# --- Placeholder ids (no AbilityStore row) — worldd rejects → GCD ROLLBACK demo ---
		# Firebolt: a cast-time spell (cast_ms>0) — exercises the real cast-bar path.
		{"ability_id": 1001, "name": "Firebolt", "icon_id": 1001, "hotkey": 3,
			"cast_ms": 1500, "triggers_gcd": true, "resource_type": 1, "resource_cost": 20, "range_m": 30.0},
		{"ability_id": 1002, "name": "Frost Nova", "icon_id": 1002, "hotkey": 4,
			"cast_ms": 0, "triggers_gcd": true, "resource_type": 1, "resource_cost": 15, "range_m": 12.0},
		{"ability_id": 1003, "name": "Renew", "icon_id": 1003, "hotkey": 5,
			"cast_ms": 0, "triggers_gcd": true, "resource_type": 1, "resource_cost": 10, "range_m": 40.0},
	]
