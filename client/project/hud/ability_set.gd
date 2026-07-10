# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the GREYBOX known-ability set for the action bar (CMB-01/UI-01, #432).
#
# ⛔ WHY THIS EXISTS — a wire-contract gap (reported, NOT faked). The client is NEVER SENT
# the character's known ability set. world.fbs teaches abilities server-side
# (TRAINER_LEARN_RESULT) and pushes a per-NPC TRAINER_LIST, but there is NO S→C
# "your abilities" / spellbook push at enter-world (no KNOWN_ABILITIES opcode) — the exact
# shape of the #439 self-vitals / #443 quest-reward-preview gaps. So until a KNOWN_ABILITIES
# contract lands, the action bar is driven by THIS documented placeholder set, seeded into
# the event bus via MeridianEventBus.seed_abilities(). The CAST_REQUEST → GCD → cast-bar
# wire path is fully REAL; only the SOURCE of the slot list is greybox.
#
# IDs ARE REAL WHERE THEY EXIST. worldd validates CAST_REQUEST.ability_id against its loaded
# AbilityStore (the mcc-compiled `content/core/idmap.lock`). Slots 1-2 carry the two abilities
# the store actually loads (minor_healing = 1, pickaxe_slam = 2) so a cast against a valid
# target resolves to CAST_START + CAST_RESULT; slots 3+ carry placeholder ids that do NOT
# resolve, so worldd cleanly rejects them (CAST_FAILED / UNKNOWN_ABILITY) — which is exactly
# how we demonstrate the D-10 optimistic-GCD ROLLBACK in the greybox client.
#
# NOTE: no shipped greybox ability has a cast time yet (both real abilities are instant,
# cast_ms = 0), so the manual run exercises only the GCD sweep + rollback; the CAST BAR path
# (a CAST_START with cast_ms > 0 → fill → clear on CAST_RESULT) is proven headlessly in
# hud/combat_verify.gd until a cast-time ability ships in content.
extends RefCounted
class_name MeridianAbilitySet


# The greybox action-bar slot list, in hotkey order (1..N). Each entry is the canonical
# ability Dictionary the event bus stores + the action bar renders:
#   { ability_id:int, name:String, icon_id:int, hotkey:int }
# `icon_id` is the placeholder art/ID hook — the action bar derives a stable placeholder
# swatch from it (no real icon art at greybox), mirroring how the trainer window renders
# "Ability #<id>" by id.
static func greybox_abilities() -> Array:
	return [
		# --- Real AbilityStore ids (idmap.lock) — resolve to CAST_START + CAST_RESULT ---
		{"ability_id": 2, "name": "Pickaxe Slam", "icon_id": 2, "hotkey": 1},   # enemy damage, range 5
		{"ability_id": 1, "name": "Minor Healing", "icon_id": 1, "hotkey": 2},  # self heal (no GCD)
		# --- Placeholder ids (no AbilityStore row) — worldd rejects → GCD ROLLBACK demo ---
		{"ability_id": 1001, "name": "Firebolt", "icon_id": 1001, "hotkey": 3},
		{"ability_id": 1002, "name": "Frost Nova", "icon_id": 1002, "hotkey": 4},
		{"ability_id": 1003, "name": "Renew", "icon_id": 1003, "hotkey": 5},
	]
