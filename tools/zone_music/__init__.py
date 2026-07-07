# SPDX-License-Identifier: Apache-2.0
"""Engine-free reference core for the ZoneMusicPlayer runtime (AUD-02, #144).

The adaptive-music runtime lives in Godot (`client/project/audio/*.gd`), but its
*pure logic* — the explore/tension/combat/silence state machine, the vertical
layer mix, the transition table, the sample-domain bar clock, and the zone->set
->stem mapping — is defined here in plain Python so it is unit-testable with
pytest (no Godot, no audio device) and is the single reference the GDScript
mirrors 1:1. The runtime and this core share `client/project/audio/
zone_music_config.json` and the TD-11 harness's `ClockConfig`, so all three
agree on the mapping and the sample grid by construction.

This is the "engine-free / testable where separable" slice the #144 task calls
for; `tests/test_zone_music.py` exercises it.
"""

from .bar_clock import (
    QUANTIZE_BAR,
    QUANTIZE_BEAT,
    QUANTIZE_IMMEDIATE,
    QUANTIZE_LOOP_END,
    ClockConfig,
    boundary_sample,
    next_bar_sample,
    next_beat_sample,
)
from .state_core import (
    COMBAT,
    EXPLORE,
    FLOOR_DB,
    FULL_DB,
    LAYERS,
    SILENCE,
    STATES,
    TENSION,
    CrossfadeSchedule,
    LayerRamp,
    TransitionRule,
    crossfade_schedule,
    director_state,
    layer_gains_db,
    rule_for,
)
from .track_map import (
    MusicSet,
    Stem,
    ZoneMusicConfigError,
    ZoneTrackMap,
    load_config,
)

__all__ = [
    "ClockConfig",
    "QUANTIZE_BAR",
    "QUANTIZE_BEAT",
    "QUANTIZE_IMMEDIATE",
    "QUANTIZE_LOOP_END",
    "boundary_sample",
    "next_bar_sample",
    "next_beat_sample",
    "COMBAT",
    "EXPLORE",
    "TENSION",
    "SILENCE",
    "STATES",
    "LAYERS",
    "FULL_DB",
    "FLOOR_DB",
    "CrossfadeSchedule",
    "LayerRamp",
    "TransitionRule",
    "crossfade_schedule",
    "director_state",
    "layer_gains_db",
    "rule_for",
    "MusicSet",
    "Stem",
    "ZoneTrackMap",
    "ZoneMusicConfigError",
    "load_config",
]
