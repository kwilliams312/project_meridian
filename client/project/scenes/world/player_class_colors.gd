# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the SINGLE shared placeholder class→color table (issue #328).
#
# The server carries each player's M0-frozen class id on the wire (world.fbs
# EntityEnter.char_class, populated authoritatively by worldd from the character
# record). Every client maps that id to a capsule color THROUGH THIS TABLE so the
# LOCAL player and every REMOTE player agree on what a class looks like — two+
# clients side by side render the same character in the same color, which is the
# whole point (tell players apart by class during multi-client testing).
#
# world.gd's local-player builder (_build_player_and_camera) and remote-player
# builder (_spawn_remote) BOTH read this one table, so there is no way for own vs.
# remote coloring to drift apart.
#
# Class ids are the M0-frozen roster (server/characters/src/roster.h `enum Class`):
#   1 = Vanguard  (front-line melee)
#   2 = Runcaller (arcane caster)
#   3 = Warden    (ranged/hybrid)
#   4 = Mender    (healer)
# id 0 = unset/unknown -> a neutral grey (a pre-#328 server, or a non-player entity
# that carries no class, never renders as a real class color).
#
# PLACEHOLDER colors only — no art assets (D-11: one placeholder model). This is a
# per-class Color constant table, nothing more.

extends RefCounted

# Neutral fallback for id 0 / any id not in the frozen roster.
const UNKNOWN := Color(0.70, 0.70, 0.72)

# The frozen class→color map. Distinct, saturated hues so classes read apart at a
# glance and at distance on the flat bootstrap plane.
const COLORS := {
	1: Color(0.86, 0.28, 0.24),  # Vanguard  — steel red   (melee)
	2: Color(0.30, 0.52, 0.95),  # Runcaller — arcane blue  (caster)
	3: Color(0.34, 0.78, 0.42),  # Warden    — forest green (ranged/hybrid)
	4: Color(0.94, 0.80, 0.28),  # Mender    — warm gold    (healer)
}


# The placeholder capsule color for a class id. Unknown/out-of-roster -> neutral grey.
static func color_for(class_id: int) -> Color:
	return COLORS.get(class_id, UNKNOWN)
