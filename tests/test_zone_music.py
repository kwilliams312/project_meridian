# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/zone_music — the engine-free ZoneMusicPlayer core (#144).

Covers the "pure logic, testable where separable" slice the task calls for:
zone->track selection, the state machine + hysteresis, the adaptive vertical
layer mix, the crossfade schedule, and the sample-domain timing-seam values the
TD-11 harness (§3.1) measures against.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from td11_music_timing.probe import ClockConfig  # noqa: E402
from zone_music import (  # noqa: E402
    COMBAT,
    EXPLORE,
    FLOOR_DB,
    FULL_DB,
    SILENCE,
    TENSION,
    crossfade_schedule,
    director_state,
    layer_gains_db,
    rule_for,
)
from zone_music.bar_clock import (  # noqa: E402
    next_bar_sample,
    next_beat_sample,
)
from zone_music.track_map import (  # noqa: E402
    ZoneMusicConfigError,
    ZoneTrackMap,
)

pytestmark = pytest.mark.unit

CFG = ClockConfig()  # 44.1 kHz, 96 BPM, 4/4 — matches music SAD §4.2


# --- zone -> track selection --------------------------------------------
def test_config_file_loads_and_parses():
    tm = ZoneTrackMap.from_file()  # the real committed config
    ms = tm.resolve_zone("zone.bootstrap")
    assert ms.set_id == "core:mus.placeholder"
    assert ms.placeholder is True
    assert ms.bpm == 96.0 and ms.beats_per_bar == 4 and ms.length_bars == 96


def test_zone_resolves_to_set_and_stems():
    tm = ZoneTrackMap.from_file()
    ms = tm.resolve_zone("zone.bootstrap")
    # 5 stems: L1, L2, L3, L4, L4 (music PRD §6 M0: L1×1, L2×1, L3×1, L4×2).
    assert len(ms.stems) == 5
    assert [s.layer for s in ms.stems] == ["L1", "L2", "L3", "L4", "L4"]
    assert ms.stems_for_layer("L4") == ms.stems[3:5]


def test_unknown_zone_falls_back_to_default():
    tm = ZoneTrackMap.from_file()
    # No such zone -> default_zone mapping, not a crash.
    assert tm.set_id_for_zone("zone.does_not_exist") == "core:mus.placeholder"


def test_content_references_ids_not_paths():
    tm = ZoneTrackMap.from_file()
    for s in tm.resolve_zone("zone.default").stems:
        assert s.asset_id.startswith("core:mus.")
        assert "/" not in s.asset_id.split(":", 1)[1]  # an ID, never a file path


def test_id_hook_path_resolver_is_injectable():
    # The #148 ID hook: asset id -> runtime resource, via an injected resolver.
    tm = ZoneTrackMap.from_file(asset_resolver=lambda aid: f"res://meridian/{aid}")
    resolved = tm.resolved_stems("core:mus.placeholder")
    assert resolved[0][1] == "res://meridian/core:mus.placeholder.explore.layer1"


def test_set_without_l1_is_rejected():
    bad = {
        "sets": {"s": {"bpm": 96, "beats_per_bar": 4, "length_bars": 96,
                       "stems": [{"layer": "L2", "asset_id": "core:mus.x"}] * 5}},
        "zones": {"z": {"set": "s"}}, "default_zone": "z",
    }
    with pytest.raises(ZoneMusicConfigError, match="no L1 bed"):
        ZoneTrackMap(bad)


def test_set_with_wrong_stem_count_is_rejected():
    bad = {
        "sets": {"s": {"bpm": 96, "beats_per_bar": 4, "length_bars": 96,
                       "stems": [{"layer": "L1", "asset_id": "core:mus.x"}] * 3}},
        "zones": {"z": {"set": "s"}}, "default_zone": "z",
    }
    with pytest.raises(ZoneMusicConfigError, match="5–7"):
        ZoneTrackMap(bad)


# --- state selection (MusicDirector reduction) --------------------------
def test_director_state_explore_when_idle():
    assert director_state(combat_flag=False, hostile_proximity=False) == EXPLORE


def test_director_state_tension_on_hostile_proximity():
    assert director_state(combat_flag=False, hostile_proximity=True) == TENSION


def test_director_state_combat_flag_beats_tension():
    assert director_state(combat_flag=True, hostile_proximity=True) == COMBAT


def test_director_state_boss_forces_combat():
    assert director_state(False, False, boss_encounter=True) == COMBAT


# --- adaptive vertical layer mix (music SAD §2.2) -----------------------
def test_explore_plays_bed_and_melody_only():
    g = layer_gains_db(EXPLORE)
    assert g["L1"] == FULL_DB and g["L2"] == FULL_DB
    assert g["L3"] == FLOOR_DB and g["L4"] == FLOOR_DB


def test_combat_adds_tension_and_combat_layers():
    g = layer_gains_db(COMBAT)
    assert g["L1"] == FULL_DB and g["L3"] == FULL_DB and g["L4"] == FULL_DB
    assert g["L2"] == FLOOR_DB  # melody ducks under combat


def test_l1_bed_is_audible_in_every_playing_state():
    for state in (EXPLORE, TENSION, COMBAT):
        assert layer_gains_db(state)["L1"] == FULL_DB  # L1 never stops


# --- transition table (music SAD §2.1) ----------------------------------
def test_combat_entry_is_beat_quantized_with_stinger():
    r = rule_for(EXPLORE, COMBAT)
    assert r.quantize == "beat"
    assert r.fade_ms == 500.0  # ≤500 ms, feels immediate
    assert r.stinger == "combat_enter"
    assert r.hysteresis_s == 0.0


def test_combat_exit_has_hysteresis_and_bar_fade():
    r = rule_for(COMBAT, EXPLORE)
    assert r.quantize == "bar"
    assert r.hysteresis_s == 4.0  # 4 s out-of-combat before fade back
    assert r.fade_bars == 2.0
    assert r.stinger == "combat_end"


def test_tension_exit_has_longer_hysteresis():
    assert rule_for(TENSION, EXPLORE).hysteresis_s == 6.0


def test_silence_is_overridden_immediately_by_combat():
    r = rule_for(SILENCE, COMBAT)
    assert r.quantize == "immediate"
    assert r.stinger == "combat_enter"


# --- crossfade schedule + timing-seam values ----------------------------
def test_next_bar_sample_matches_shadow_clock_formula():
    # 96 BPM 4/4 @ 44.1k -> 110250 samples/bar. A request mid-bar rounds up to
    # the next bar boundary — the predicted_boundary_sample of a TransitionEvent.
    spb = CFG.samples_per_bar
    assert spb == pytest.approx(110_250.0)
    assert next_bar_sample(0, CFG) == 0
    assert next_bar_sample(1, CFG) == 110_250
    assert next_bar_sample(110_250, CFG) == 110_250
    assert next_bar_sample(110_251, CFG) == 220_500


def test_next_beat_sample_for_combat_entry():
    spbeat = CFG.samples_per_beat  # 27562.5
    assert next_beat_sample(1, CFG) == int(round(spbeat))
    assert next_beat_sample(int(spbeat) + 1, CFG) == int(round(2 * spbeat))


def test_explore_to_combat_schedule_is_beat_quantized_and_ramps_l3_l4_up():
    req = 5_000
    sched = crossfade_schedule(EXPLORE, COMBAT, CFG, req)
    assert sched.quantize == "beat"
    assert sched.boundary_sample == next_beat_sample(req, CFG)
    # 500 ms fade -> 22050 samples at 44.1k.
    assert sched.fade_samples == 22_050
    # L3 and L4 rise (silence -> full); L1 unchanged (no ramp); L2 ducks out.
    assert sched.ramp_for("L1") is None  # bed already full in both states
    assert sched.ramp_for("L2").rising is False  # melody ducks under combat
    l3 = sched.ramp_for("L3")
    assert l3 is not None and l3.rising and l3.start_db == FLOOR_DB and l3.end_db == FULL_DB
    assert l3.begin_sample == sched.boundary_sample
    assert l3.end_sample == sched.boundary_sample + 22_050
    assert sched.stinger == "combat_enter"


def test_combat_to_explore_schedule_fades_l3_l4_out_and_l2_in():
    sched = crossfade_schedule(COMBAT, EXPLORE, CFG, 12_345)
    assert sched.quantize == "bar"
    # 2-bar equal-power fade.
    assert sched.fade_samples == int(round(2 * CFG.samples_per_bar))
    assert sched.ramp_for("L2").rising is True  # melody returns
    assert sched.ramp_for("L3").rising is False  # tension out
    assert sched.ramp_for("L4").rising is False  # combat out


def test_schedule_boundary_never_before_request():
    for req in (0, 1, 55_000, 110_249, 110_250, 300_001):
        s = crossfade_schedule(EXPLORE, TENSION, CFG, req)
        assert s.boundary_sample >= req  # a next-bar flip fires on/after the request


def test_silence_schedule_uses_loop_length():
    sched = crossfade_schedule(EXPLORE, SILENCE, CFG, 1, length_bars=96)
    loop = int(round(96 * CFG.samples_per_bar))
    assert sched.boundary_sample == loop  # first loop boundary at/after sample 1
