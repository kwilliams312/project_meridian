# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the single power_type -> bar-color table for HUD unit frames
# (UI-01, #431). The server tags every unit's secondary pool with a PowerType
# (world.fbs / schema/net/world.fbs): NONE / MANA / ENERGY / RAGE. The client colors
# the power bar by that id through this ONE table so the player frame, the target
# frame, and any future nameplate all render the same pool in the same color — the
# power-bar analogue of the shared class->color table (scenes/world/player_class_colors.gd,
# #328). Append within a major; never renumber (the ids are the wire PowerType enum).
class_name MeridianPowerColors
extends RefCounted

# world.fbs PowerType ordinals (kept in sync with schema/net/world.fbs `PowerType`).
const NONE := 0
const MANA := 1
const ENERGY := 2
const RAGE := 3

# Bar colors, chosen for the greybox HUD (readable on the world backdrop, distinct
# from the red health bar). Mana = blue, Energy = yellow, Rage = orange-red.
const _COLORS := {
	MANA: Color(0.25, 0.45, 0.95),
	ENERGY: Color(0.95, 0.85, 0.20),
	RAGE: Color(0.90, 0.35, 0.20),
}

# The bar color for a power_type. NONE (or any unknown id) returns a neutral grey —
# the caller HIDES the power bar for NONE, so this is only a defensive fallback.
static func color_for(power_type: int) -> Color:
	return _COLORS.get(power_type, Color(0.45, 0.45, 0.45))

# A unit has a secondary power pool to draw iff power_type is a known non-NONE id
# with a positive cap. (A basic melee creature is PowerType NONE / max_power 0.)
static func has_power(power_type: int, max_power: int) -> bool:
	return power_type != NONE and max_power > 0
