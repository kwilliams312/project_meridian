# SPDX-License-Identifier: Apache-2.0
"""Engine-free reference core for the SFX trigger runtime (AUD-01 seed, #148).

The SFX runtime lives in Godot (`client/project/audio/sfx_*.gd`), but its *pure
logic* — the event -> sfx-id registry, the content-ID hook (asset id -> runtime
resource), and the category -> bus/group routing table (music SAD §2.5) — is
defined here in plain Python so it is unit-testable with pytest (no Godot, no
audio device) and is the single reference the GDScript mirrors 1:1. The runtime
and this core share `client/project/audio/sfx_config.json`, so both agree on the
event map and routing by construction.

This is the SFX counterpart of `tools/zone_music` (#144): music resolves
zone->set->stem, SFX resolves event->sfx-id->one-shot, through the same ID hook
(music SAD §5). `tests/test_zone_sfx.py` exercises it.
"""

from .event_map import (
    Category,
    SfxConfigError,
    SfxDef,
    SfxEventMap,
    load_config,
)

__all__ = [
    "Category",
    "SfxConfigError",
    "SfxDef",
    "SfxEventMap",
    "load_config",
]
