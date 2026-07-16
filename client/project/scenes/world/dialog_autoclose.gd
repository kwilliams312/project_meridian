# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — the PURE NPC dialog auto-close policy (#851). While a gossip / vendor /
# trainer window is open, the world scene closes it once the player walks out of the NPC's
# interaction reach (the same UX as pressing Esc, driven by distance). This module holds the
# reach constants + the distance decision as dependency-free statics so world.gd delegates to
# it and scenes/world/dialog_autoclose_verify.gd can exercise the policy WITHOUT loading the
# whole world scene (which pulls in GDExtension net types unavailable to a headless verify).
#
# The reach MIRRORS the server gate (#842): server/worldd/world_dispatch.cpp
# kNpcInteractionRangeM == creature_ai.h kCreatureBasicAttackRangeM == 5 m, measured as a
# HORIZONTAL (planar, height-ignoring) distance. Keep RANGE_M in sync if that constant moves.
# A small hysteresis — interactions open at 5 m but only auto-close past 5.5 m — keeps a
# player jittering at the boundary from flickering the window. The server stays authoritative
# (it already refuses out-of-range gossip/accept/turn-in); this is purely the client's view.
extends RefCounted

const RANGE_M := 5.0    # interaction reach — mirrors the server gate (#842)
const CLOSE_M := 5.5    # auto-close past here (RANGE_M + hysteresis)


# True iff the player is beyond `close_range_m` of the NPC, measured HORIZONTALLY (the X/Z
# ground plane, ignoring height — worldd is Z-up, Godot Y-up, so height is Godot Y; this
# matches the server's horizontal_distance gate). Squared-distance compare — no sqrt, no
# allocation.
static func should_close(player_pos: Vector3, npc_pos: Vector3, close_range_m: float) -> bool:
	var dx := player_pos.x - npc_pos.x
	var dz := player_pos.z - npc_pos.z
	return (dx * dx + dz * dz) > (close_range_m * close_range_m)
