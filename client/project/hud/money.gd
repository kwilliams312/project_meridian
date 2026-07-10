# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — copper money formatting (ECO-01, #441). All money on the wire is
# int64 COPPER (never a float); the classic split is 100 copper = 1 silver, 100 silver =
# 1 gold. This one helper renders a copper amount as "Ng Ms Kc" (omitting zero higher
# denominations) so the loot / vendor / trainer / bags windows format money identically.
#
# Pure + static (no state, no Node) — trivially unit-testable headlessly (econ_verify.gd).
class_name MeridianMoney
extends RefCounted

const COPPER_PER_SILVER := 100
const COPPER_PER_GOLD := 100 * 100  # 10000 copper = 1 gold


## Format a copper amount as a compact "g/s/c" string. Negative guards to 0 (a balance is
## never negative on the wire). 0 renders "0c".
static func format_copper(copper: int) -> String:
	if copper <= 0:
		return "0c"
	var gold := copper / COPPER_PER_GOLD
	var silver := (copper % COPPER_PER_GOLD) / COPPER_PER_SILVER
	var cop := copper % COPPER_PER_SILVER
	var parts: Array = []
	if gold > 0:
		parts.append("%dg" % gold)
	if silver > 0:
		parts.append("%ds" % silver)
	if cop > 0 or parts.is_empty():
		parts.append("%dc" % cop)
	return " ".join(parts)
