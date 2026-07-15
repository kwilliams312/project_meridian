"""Guard: schema/content/skeleton.defs.yaml `raceName`'s CORE PREFIX must mirror the
canonical server/characters/src/roster.h race ids (the `kRace<Name> = <id>`
constants), lowercased, in ROSTER ID ORDER — and pack-authored race names may follow.

WHY this guard exists (final-review epic #451, Important-a — the off-by-one trap):
`roster.h` numbers races 1-based (`kRaceArdent = 1 … kRaceEmberkin = 4`), but the
code generator (`tools/schema_gen`) lowers the `raceName` enum to a 0-based ordinal
`RaceName` in `content_types.gen.hpp` / `ContentTypes.g.cs`. So a naive
`(RaceName)race` cast on a wire `race:uint8` (which carries the 1-based roster id,
spec ②) is off by one, and if the two lists drift in CONTENT or ORDER the mapping
silently corrupts (e.g. a Dolmen character renders as Ardent). This test freezes the
core prefix so a future roster.h append/reorder that forgets skeleton.defs fails
loudly here instead of shipping a mis-render. (No `(RaceName)race` ordinal cast
exists in client/server/codex today — the runtime resolves race→name via the pack
Roster string, not the enum ordinal — so this is defense-in-depth for the core set.)

MULTI-PACK ROSTERS (chibi theme, design 2026-07-14-chibi §4/§7 — #695/#708). Race
identity moved to PACK DATA: each pack ships its own `Roster` keyed by `roster_id`,
and roster ids are reused ACROSS packs (the chibi theme's 6 colour races are
roster_ids 1–6 in the CHIBI roster, independent of core's ardent=1…emberkin=4). A
single global enum ordinal therefore CANNOT equal roster_id for every pack — the
`RaceName_ordinal == roster_id - 1` relation holds ONLY for the compiled core roster
(the enum prefix). Pack-authored names (red/green/blue/yellow/gold/silver) are
appended to the `raceName` VOCABULARY so their appearance catalogs validate; their
ordinal is decoupled from their per-pack roster_id BY DESIGN. The client/server never
cast a pack race by ordinal (they use the pack Roster), so this is safe — the guard
below asserts the invariant on the core prefix and only requires the pack names to be
well-formed, distinct, lowercase vocabulary entries.
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


def test_skeleton_racename_core_prefix_matches_roster_ids_in_order():
    """The core (roster.h) races are the enum PREFIX, in roster-id order — so the
    `RaceName_ordinal == roster_id - 1` cast stays correct for the compiled core
    roster. Pack-authored names (chibi colours) follow and are not ordinal-mapped."""
    roster = _roster_race_names_in_id_order()
    skeleton = _skeleton_race_names()
    prefix = skeleton[: len(roster)]
    assert prefix == roster, (
        "skeleton.defs.yaml raceName core prefix drifted from roster.h Race enum.\n"
        f"  roster.h (id order, lowercased): {roster}\n"
        f"  skeleton raceName prefix:        {prefix}\n"
        f"  full skeleton raceName enum:     {skeleton}\n"
        "The first N raceName entries MUST be the roster.h races in roster-id order "
        "(append/reorder BOTH together) — a wire race:uint8 (1-based roster id) maps "
        "to RaceName ordinal (0-based) as roster_id-1 for the CORE roster, so the "
        "prefix order must agree exactly. Pack race names are appended AFTER."
    )


def test_skeleton_racename_pack_suffix_is_well_formed():
    """Names appended after the core prefix (pack-authored race vocab, e.g. the chibi
    colours) must be distinct, lowercase, non-empty vocabulary entries. They validate
    appearance `race` fields but are resolved via the pack Roster, not the enum ordinal."""
    roster = _roster_race_names_in_id_order()
    skeleton = _skeleton_race_names()
    pack_names = skeleton[len(roster) :]
    assert len(set(skeleton)) == len(skeleton), (
        f"duplicate raceName entries: {skeleton}"
    )
    for name in pack_names:
        assert (
            name and name == name.lower() and re.fullmatch(r"[a-z][a-z0-9_]*", name)
        ), f"pack race name {name!r} is not a well-formed lowercase vocabulary entry"


def test_roster_ids_are_dense_and_one_based():
    """The roster_id-1 == RaceName_ordinal relation only holds if roster ids are a
    dense 1..N run. Guard that invariant too, so a gap/renumber trips here."""
    ids = sorted(idv for _, idv in _roster_race_constants())
    assert ids == list(range(1, len(ids) + 1)), (
        f"roster.h race ids are not a dense 1..N run: {ids} — the 0-based generated "
        "enum ↔ 1-based roster id mapping (ordinal == id-1) would break."
    )
