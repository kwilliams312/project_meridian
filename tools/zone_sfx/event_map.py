# SPDX-License-Identifier: Apache-2.0
"""Gameplay/UI event -> SFX-asset-id -> runtime resource — engine-free.

This is the SFX counterpart of `tools/zone_music/track_map.py` (#144): the
music track resolves *zone -> set -> stem*; the SFX track resolves *event -> sfx
id -> one-shot*, through the SAME content-ID hook (music SAD §5, "data-driven
SFX resolution"). The mapping is data, never hardcoded: it is read from
`client/project/audio/sfx_config.json`, the *same* file the GDScript
`sfx_event_map.gd` loads, so the reference core and the runtime agree by
construction.

Two id sources feed one hook:

* **Client-authored events** (footsteps by surface, UI clicks, the login sting)
  have no content object, so this config's `events` table maps a stable event
  key to a `sfx.*` id.
* **Content-carried ids** (an ability's `audio_visual.cast_sfx` / `impact_sfx`,
  an NPC `sound_set`) already carry their `sfx.*` id in `/content`. They resolve
  through the *identical* `resolved_sfx()` hook — there is no parallel path.

Content references stable asset IDs only (music PRD §7); an injectable
`asset_resolver` turns each `sfx.*` id into a runtime resource — at M0 the
resolver is the placeholder one-shot factory, so no audio file is ever referenced
by path. This module is the reference the GDScript `SfxEventMap` mirrors and the
thing `tests/test_zone_sfx.py` exercises.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

# Canonical config location (repo-relative), used when a caller passes no path.
_DEFAULT_CONFIG = (
    Path(__file__).resolve().parents[2]
    / "client"
    / "project"
    / "audio"
    / "sfx_config.json"
)

# An `sfx.*` asset id, optionally namespaced (`core:`). Mirrors `sfxRef` in
# schema/content/common.defs.yaml — the grammar content and this config share.
_SFX_ID = re.compile(r"^([a-z][a-z0-9_]{1,31}:)?sfx\.[a-z0-9_]+(\.[a-z0-9_]+)*$")

_VALID_ATTENUATION = frozenset({"small", "medium", "large", "global", "ui2d"})


@dataclass(frozen=True)
class SfxDef:
    """One placeholder one-shot: its stable asset id, its routing category, and
    the synthesis parameters the placeholder factory turns into a tone."""

    sfx_id: str
    category: str
    attenuation: str
    placeholder_tone_hz: float | None = None
    placeholder_ms: int | None = None


@dataclass(frozen=True)
class Category:
    """A concurrency-group + bus routing entry (music SAD §2.5 bus tree).

    `cap` is the M1 voice-manager concurrency cap (PRD §3.4); it is carried as
    data here but not enforced at M0 (there is no voice manager yet — the M0
    system is routing + trigger only)."""

    name: str
    bus: str
    group: str
    cap: int


class SfxConfigError(ValueError):
    """A structural problem in sfx_config.json (mirrors the TLS-07 reference
    lint, music SAD §4.4 — caught at load, not at trigger time)."""


def load_config(path: str | Path | None = None) -> dict:
    """Load and JSON-parse the SFX config (default: the canonical file)."""
    p = Path(path) if path is not None else _DEFAULT_CONFIG
    with p.open(encoding="utf-8") as f:
        return json.load(f)


class SfxEventMap:
    """Resolves gameplay/UI events to SFX asset ids, and asset ids to resources.

    `asset_resolver` maps a `sfx.*` asset id to whatever the caller wants the
    one-shot to become (a resource path, a placeholder descriptor, ...). It
    defaults to identity so the pure mapping is testable without a runtime.
    """

    def __init__(
        self,
        config: dict,
        asset_resolver: Callable[[str], object] | None = None,
    ) -> None:
        self._cfg = config
        self._resolver = asset_resolver or (lambda sfx_id: sfx_id)
        self._categories: dict[str, Category] = {}
        self._sfx: dict[str, SfxDef] = {}
        self._events: dict[str, str] = {}
        self._parse()

    @classmethod
    def from_file(
        cls,
        path: str | Path | None = None,
        asset_resolver: Callable[[str], object] | None = None,
    ) -> "SfxEventMap":
        return cls(load_config(path), asset_resolver)

    # --- parsing / validation --------------------------------------------
    def _parse(self) -> None:
        for name, raw in self._cfg.get("categories", {}).items():
            if name.startswith("_"):
                continue
            self._categories[name] = Category(
                name=name,
                bus=str(raw["bus"]),
                group=str(raw["group"]),
                cap=int(raw["cap"]),
            )
        if not self._categories:
            raise SfxConfigError("config defines no SFX categories")

        for sfx_id, raw in self._cfg.get("sfx", {}).items():
            if sfx_id.startswith("_"):
                continue
            self._sfx[sfx_id] = self._parse_sfx(sfx_id, raw)
        if not self._sfx:
            raise SfxConfigError("config defines no SFX entries")

        for event_key, sfx_id in self._cfg.get("events", {}).items():
            if event_key.startswith("_"):
                continue
            if sfx_id not in self._sfx:
                raise SfxConfigError(
                    f"event {event_key!r} -> unknown sfx id {sfx_id!r}"
                )
            self._events[event_key] = sfx_id

    def _parse_sfx(self, sfx_id: str, raw: dict) -> SfxDef:
        if not _SFX_ID.match(sfx_id):
            raise SfxConfigError(
                f"sfx id {sfx_id!r} is not a valid sfx.* id (an ID, never a path)"
            )
        category = str(raw["category"])
        if category not in self._categories:
            raise SfxConfigError(
                f"sfx {sfx_id}: category {category!r} not in "
                f"{sorted(self._categories)}"
            )
        attenuation = str(raw["attenuation"])
        if attenuation not in _VALID_ATTENUATION:
            raise SfxConfigError(
                f"sfx {sfx_id}: attenuation {attenuation!r} not in "
                f"{sorted(_VALID_ATTENUATION)}"
            )
        tone = raw.get("placeholder_tone_hz")
        ms = raw.get("placeholder_ms")
        return SfxDef(
            sfx_id=sfx_id,
            category=category,
            attenuation=attenuation,
            placeholder_tone_hz=float(tone) if tone is not None else None,
            placeholder_ms=int(ms) if ms is not None else None,
        )

    # --- resolution ------------------------------------------------------
    @property
    def placeholder(self) -> bool:
        return bool(self._cfg.get("placeholder", False))

    def sfx_ids(self) -> tuple[str, ...]:
        return tuple(self._sfx)

    def event_keys(self) -> tuple[str, ...]:
        return tuple(self._events)

    def sfx_entry(self, sfx_id: str) -> SfxDef:
        if sfx_id not in self._sfx:
            raise KeyError(f"unknown sfx id {sfx_id!r}")
        return self._sfx[sfx_id]

    def category(self, name: str) -> Category:
        if name not in self._categories:
            raise KeyError(f"unknown SFX category {name!r}")
        return self._categories[name]

    def resolve_event(self, event_key: str) -> str:
        """Client-authored event key -> its `sfx.*` id (music SAD §5)."""
        if event_key not in self._events:
            raise KeyError(f"no SFX mapped for event {event_key!r}")
        return self._events[event_key]

    def footstep_event(self, surface: str) -> str:
        """Surface tag -> the `footstep.<surface>` event key (music SAD §5:
        physics-material -> surface tag -> foley footstep variation group)."""
        return f"footstep.{surface}"

    def bus_for(self, sfx_id: str) -> str:
        """The AudioServer bus this id routes to (music SAD §2.5 bus tree)."""
        return self.category(self.sfx_entry(sfx_id).category).bus

    def group_for(self, sfx_id: str) -> str:
        """The concurrency group this id belongs to (enforced at M1)."""
        return self.category(self.sfx_entry(sfx_id).category).group

    def resolved_sfx(self, sfx_id: str) -> tuple[SfxDef, object]:
        """The SFX id hook path (#148): asset id -> runtime resource, via the
        injected resolver. Identical for client-authored and content-carried ids
        — the SAME hook the music track uses for stems (`resolved_stems`)."""
        return (self.sfx_entry(sfx_id), self._resolver(sfx_id))

    def resolve_content_sfx(self, sfx_id: str) -> object:
        """Resolve a content-carried id (ability `cast_sfx`/`impact_sfx`, NPC
        `sound_set`) through the same hook. Content need not be pre-declared in
        this config's `sfx` table — the hook resolves any valid `sfx.*` id."""
        if not _SFX_ID.match(sfx_id):
            raise SfxConfigError(f"{sfx_id!r} is not a valid sfx.* id")
        return self._resolver(sfx_id)
