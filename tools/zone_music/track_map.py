# SPDX-License-Identifier: Apache-2.0
"""Zone -> music-set -> stem resolution — config-driven, engine-free.

The zone->track mapping is data (music SAD §6.1), never hardcoded: it is read
from `client/project/audio/zone_music_config.json`, the *same* file the GDScript
`zone_track_map.gd` loads, so the reference core and the runtime agree by
construction. Content references stable asset IDs only (music PRD §7); an
injectable `asset_resolver` turns each `mus.*` id into a runtime resource — at M0
the resolver is the placeholder factory, so no audio file is ever referenced by
path.

This module is the reference the GDScript `ZoneTrackMap` mirrors and the thing
`tests/test_zone_music.py` exercises for the "zone->track selection" requirement.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

# Canonical config location (repo-relative), used when a caller passes no path.
_DEFAULT_CONFIG = (
    Path(__file__).resolve().parents[2]
    / "client"
    / "project"
    / "audio"
    / "zone_music_config.json"
)

_VALID_LAYERS = frozenset({"L1", "L2", "L3", "L4", "boss", "stinger"})


@dataclass(frozen=True)
class Stem:
    """One stem of a set: its layer role and the stable asset ID content uses."""

    layer: str
    asset_id: str
    placeholder_tone_hz: float | None = None


@dataclass(frozen=True)
class MusicSet:
    """A resolved zone music set: musical metadata + the ordered stem list."""

    set_id: str
    bpm: float
    beats_per_bar: int
    length_bars: int
    key: str
    stems: tuple[Stem, ...]
    placeholder: bool = False

    def stems_for_layer(self, layer: str) -> tuple[Stem, ...]:
        return tuple(s for s in self.stems if s.layer == layer)

    @property
    def asset_ids(self) -> tuple[str, ...]:
        return tuple(s.asset_id for s in self.stems)


class ZoneMusicConfigError(ValueError):
    """A structural problem in zone_music_config.json (mirrors the TLS-07
    stem-set-consistency lint, music SAD §4.4 — caught at load, not at play)."""


def load_config(path: str | Path | None = None) -> dict:
    """Load and JSON-parse the zone-music config (default: the canonical file)."""
    p = Path(path) if path is not None else _DEFAULT_CONFIG
    with p.open(encoding="utf-8") as f:
        return json.load(f)


class ZoneTrackMap:
    """Resolves a zone id to its music set + stems from the config.

    `asset_resolver` maps a `mus.*` asset id to whatever the caller wants the
    stem to become (a resource path, a placeholder descriptor, ...). It defaults
    to identity so the pure mapping is testable without a runtime.
    """

    def __init__(
        self,
        config: dict,
        asset_resolver: Callable[[str], object] | None = None,
    ) -> None:
        self._cfg = config
        self._resolver = asset_resolver or (lambda asset_id: asset_id)
        self._sets: dict[str, MusicSet] = {}
        self._parse()

    @classmethod
    def from_file(
        cls,
        path: str | Path | None = None,
        asset_resolver: Callable[[str], object] | None = None,
    ) -> "ZoneTrackMap":
        return cls(load_config(path), asset_resolver)

    # --- parsing / validation --------------------------------------------
    def _parse(self) -> None:
        sets = self._cfg.get("sets", {})
        for set_id, raw in sets.items():
            if set_id.startswith("_"):
                continue
            self._sets[set_id] = self._parse_set(set_id, raw)
        if not self._sets:
            raise ZoneMusicConfigError("config defines no music sets")

    def _parse_set(self, set_id: str, raw: dict) -> MusicSet:
        stems: list[Stem] = []
        for s in raw.get("stems", []):
            layer = s["layer"]
            if layer not in _VALID_LAYERS:
                raise ZoneMusicConfigError(
                    f"set {set_id}: stem layer {layer!r} not in {sorted(_VALID_LAYERS)}"
                )
            stems.append(
                Stem(
                    layer=layer,
                    asset_id=s["asset_id"],
                    placeholder_tone_hz=s.get("placeholder_tone_hz"),
                )
            )
        # Stem-set consistency (music SAD §4.4): every set needs an L1 bed and
        # 5–7 stems for a zone-scale set. Enforced at load like the TLS-07 lint.
        if not any(st.layer == "L1" for st in stems):
            raise ZoneMusicConfigError(f"set {set_id}: no L1 bed stem (SAD §2.2)")
        if not 5 <= len(stems) <= 7:
            raise ZoneMusicConfigError(
                f"set {set_id}: {len(stems)} stems, expected 5–7 (music PRD §2.1)"
            )
        return MusicSet(
            set_id=set_id,
            bpm=float(raw["bpm"]),
            beats_per_bar=int(raw["beats_per_bar"]),
            length_bars=int(raw["length_bars"]),
            key=str(raw.get("key", "")),
            stems=tuple(stems),
            placeholder=bool(raw.get("placeholder", self._cfg.get("placeholder", False))),
        )

    # --- resolution ------------------------------------------------------
    def set_id_for_zone(self, zone_id: str) -> str:
        """The default music set id for `zone_id` (falls back to default_zone)."""
        zones = self._cfg.get("zones", {})
        entry = zones.get(zone_id)
        if entry is None:
            default_zone = self._cfg.get("default_zone")
            entry = zones.get(default_zone) if default_zone else None
        if entry is None:
            raise KeyError(f"no music set mapped for zone {zone_id!r} and no default")
        return entry["set"]

    def default_state_for_zone(self, zone_id: str) -> str:
        zones = self._cfg.get("zones", {})
        entry = zones.get(zone_id) or zones.get(self._cfg.get("default_zone", ""), {})
        return entry.get("default_state", "explore")

    def music_set(self, set_id: str) -> MusicSet:
        if set_id not in self._sets:
            raise KeyError(f"unknown music set {set_id!r}")
        return self._sets[set_id]

    def resolve_zone(self, zone_id: str) -> MusicSet:
        """Zone id -> the fully resolved `MusicSet` (the zone->track selection)."""
        return self.music_set(self.set_id_for_zone(zone_id))

    def resolved_stems(self, set_id: str) -> tuple[tuple[Stem, object], ...]:
        """Each stem paired with what `asset_resolver` maps its asset id to —
        this is the ID hook path (#148): asset id -> runtime resource."""
        return tuple((s, self._resolver(s.asset_id)) for s in self.music_set(set_id).stems)
