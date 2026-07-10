"""Guard: schema/content/skeleton.defs.yaml `raceName` must mirror
server/characters/src/roster.h `enum class Race`, lowercased, in ROSTER ID ORDER.

WHY this guard exists (final-review epic #451, Important-a — the off-by-one trap):
`roster.h` numbers races 1-based (`kArdent = 1 … kEmberkin = 4`), but the code
generator (`tools/schema_gen`) lowers the `raceName` enum to a 0-based ordinal
`RaceName` in `content_types.gen.hpp` / `ContentTypes.g.cs`. So a naive
`(RaceName)race` cast on a wire `race:uint8` (which carries the 1-based roster id,
spec ②) is off by one, and if the two lists ever drift in CONTENT or ORDER the
mapping silently corrupts (e.g. a Dolmen character renders as Ardent). The enum
values are safe today only because the two lists agree name-for-name in order;
this test freezes that agreement so a future race append to one file but not the
other — or a reorder — fails loudly here instead of shipping a mis-render.

The mapping itself is `RaceName_ordinal == roster_id - 1` (0-based enum vs 1-based
roster). This test asserts the ordering that makes that relation hold.
"""

import re
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
ROSTER_H = REPO / "server" / "characters" / "src" / "roster.h"
SKELETON_DEFS = REPO / "schema" / "content" / "skeleton.defs.yaml"


def _roster_race_names_in_id_order() -> list[str]:
    """Parse `enum class Race { kArdent = 1, ... };` → lowercased names ordered by id.

    Isolates the `Race` enum block only (roster.h also declares `enum class Class`
    with the same `k<Name> = <n>` shape), then returns names sorted by their id.
    """
    text = ROSTER_H.read_text()
    m = re.search(r"enum\s+class\s+Race\s*:[^{]*\{(.*?)\}\s*;", text, re.DOTALL)
    assert m, "could not find `enum class Race { ... };` in roster.h"
    body = m.group(1)
    # Each member: `kArdent = 1,` (ignore trailing line comments).
    members = re.findall(r"\bk([A-Za-z0-9]+)\s*=\s*(\d+)", body)
    assert members, "no `k<Name> = <id>` members parsed from the Race enum"
    by_id = sorted(members, key=lambda nm_id: int(nm_id[1]))
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
    text = ROSTER_H.read_text()
    m = re.search(r"enum\s+class\s+Race\s*:[^{]*\{(.*?)\}\s*;", text, re.DOTALL)
    assert m
    ids = [int(i) for _, i in re.findall(r"\bk([A-Za-z0-9]+)\s*=\s*(\d+)", m.group(1))]
    assert ids == list(range(1, len(ids) + 1)), (
        f"roster.h Race ids are not a dense 1..N run: {ids} — the 0-based generated "
        "enum ↔ 1-based roster id mapping (ordinal == id-1) would break."
    )
