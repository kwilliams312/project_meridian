"""Guard: schema/content/skeleton.defs.yaml `raceName` must mirror the canonical
server/characters/src/roster.h race ids (the `kRace<Name> = <id>` constants),
lowercased, in ROSTER ID ORDER.

WHY this guard exists (final-review epic #451, Important-a — the off-by-one trap):
`roster.h` numbers races 1-based (`kRaceArdent = 1 … kRaceEmberkin = 4`), but the
code generator (`tools/schema_gen`) lowers the `raceName` enum to a 0-based ordinal
`RaceName` in `content_types.gen.hpp` / `ContentTypes.g.cs`. So a naive
`(RaceName)race` cast on a wire `race:uint8` (which carries the 1-based roster id,
spec ②) is off by one, and if the two lists ever drift in CONTENT or ORDER the
mapping silently corrupts (e.g. a Dolmen character renders as Ardent). The enum
values are safe today only because the two lists agree name-for-name in order;
this test freezes that agreement so a future race append to one file but not the
other — or a reorder — fails loudly here instead of shipping a mis-render.

SP2.5 #695 retired the compiled `enum class Race` in favour of a runtime Roster
loaded from pack data (Ardent/Dolmen roster ids now live in the pack as the
`roster_id` field; Sylvane/Emberkin remain a compiled fallback). The canonical
id↔name list roster.h still owns — and this guard parses — is the `kRace<Name>`
id-constant block, which documents the full M0 race set (pack + fallback) in one
place. The invariant is unchanged: `RaceName_ordinal == roster_id - 1`.
"""

import re
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
ROSTER_H = REPO / "server" / "characters" / "src" / "roster.h"
SKELETON_DEFS = REPO / "schema" / "content" / "skeleton.defs.yaml"


def _roster_race_constants() -> list[tuple[str, int]]:
    """Parse roster.h's `kRace<Name> = <id>;` constants → [(Name, id), ...].

    Matches the id constants (kRaceArdent = 1, …), excluding the `kRaceCount`
    tally which shares the prefix but is not a roster entry.
    """
    text = ROSTER_H.read_text()
    members = [
        (name, int(idv))
        for name, idv in re.findall(r"\bkRace([A-Za-z0-9]+)\s*=\s*(\d+)", text)
        if name != "Count"
    ]
    assert members, "no `kRace<Name> = <id>` constants parsed from roster.h"
    return members


def _roster_race_names_in_id_order() -> list[str]:
    """roster.h race names lowercased, ordered by roster id."""
    by_id = sorted(_roster_race_constants(), key=lambda nm_id: nm_id[1])
    return [name.lower() for name, _ in by_id]


def _skeleton_race_names() -> list[str]:
    doc = yaml.safe_load(SKELETON_DEFS.read_text())
    return list(doc["$defs"]["raceName"]["enum"])


def test_skeleton_racename_matches_roster_ids_in_order():
    roster = _roster_race_names_in_id_order()
    skeleton = _skeleton_race_names()
    assert skeleton == roster, (
        "skeleton.defs.yaml raceName drifted from roster.h Race enum.\n"
        f"  roster.h (id order, lowercased): {roster}\n"
        f"  skeleton.defs raceName enum:     {skeleton}\n"
        "Append/reorder BOTH together — a wire race:uint8 (1-based roster id) maps "
        "to RaceName ordinal (0-based) as roster_id-1, so order must agree exactly."
    )


def test_roster_ids_are_dense_and_one_based():
    """The roster_id-1 == RaceName_ordinal relation only holds if roster ids are a
    dense 1..N run. Guard that invariant too, so a gap/renumber trips here."""
    ids = sorted(idv for _, idv in _roster_race_constants())
    assert ids == list(range(1, len(ids) + 1)), (
        f"roster.h race ids are not a dense 1..N run: {ids} — the 0-based generated "
        "enum ↔ 1-based roster id mapping (ordinal == id-1) would break."
    )
