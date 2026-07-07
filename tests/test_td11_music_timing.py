# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/td11_music_timing — the TD-11 measurement harness (#145).

Verifies the sample-domain math, the mock's physics, the §3.1 gate evaluation,
determinism, and the report round-trip.
"""

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from td11_music_timing.mock_source import (  # noqa: E402
    PROFILES,
    SampleClockModel,
    VarianceModel,
)
from td11_music_timing.probe import (  # noqa: E402
    ClockConfig,
    DriftSample,
    GodotTimingSource,
    TimingSource,
    TransitionEvent,
)
from td11_music_timing.report import build_summary, human_summary  # noqa: E402
from td11_music_timing.stats import (  # noqa: E402
    STEM_DRIFT_MS,
    TRANSITION_ERROR_MS,
    Distribution,
    analyze,
    percentile,
)

pytestmark = pytest.mark.unit


# --- ClockConfig sample math --------------------------------------------
def test_samples_per_bar_matches_bpm_and_meter():
    cfg = ClockConfig(sample_rate=44_100, bpm=96.0, beats_per_bar=4)
    # 96 BPM -> 0.625 s/beat -> 27562.5 samples/beat -> 110250 samples/bar
    assert cfg.samples_per_beat == pytest.approx(27_562.5)
    assert cfg.samples_per_bar == pytest.approx(110_250.0)


def test_samples_to_ms_and_beats_roundtrip():
    cfg = ClockConfig()
    assert cfg.samples_to_ms(cfg.ms_to_samples(10.0)) == pytest.approx(10.0)
    assert cfg.samples_to_beats(cfg.samples_per_beat) == pytest.approx(1.0)


# --- percentile ----------------------------------------------------------
def test_percentile_linear_interpolation():
    vals = [0.0, 10.0, 20.0, 30.0, 40.0]
    assert percentile(vals, 0) == 0.0
    assert percentile(vals, 100) == 40.0
    assert percentile(vals, 50) == 20.0
    assert percentile(vals, 95) == pytest.approx(38.0)


def test_percentile_empty_is_zero():
    assert percentile([], 95) == 0.0


def test_distribution_of_reports_min_median_p95_max():
    d = Distribution.of([1.0, 2.0, 3.0, 4.0], "ms")
    assert (d.min, d.max, d.count, d.unit) == (1.0, 4.0, 4, "ms")
    assert d.median == pytest.approx(2.5)


# --- TransitionEvent / DriftSample derived fields -----------------------
def test_transition_error_is_actual_minus_predicted():
    e = TransitionEvent(0, 1000, 2000, 2050, "explore", "combat")
    assert e.error_samples == 50


def test_drift_sample_lockstep_and_shadow_deltas():
    d = DriftSample(bar_index=3, stem_positions=(100, 105, 98), shadow_clock_sample=100)
    assert d.stem_lockstep_drift_samples == 7  # 105 - 98
    assert d.stack_vs_shadow_drift_samples == 0  # median 100 vs shadow 100


# --- mock physics --------------------------------------------------------
def test_mock_transition_error_never_negative():
    # A next-bar transition can only fire on or after the boundary.
    src = SampleClockModel(variance=PROFILES["load"], seed=3)
    for e in src.run_transition_trials(300):
        assert e.error_samples >= 0


def test_mock_quantization_floor_bounded_by_mix_block_under_noload():
    cfg = ClockConfig(mix_block_frames=128)
    src = SampleClockModel(cfg=cfg, variance=VarianceModel(), seed=0)
    events = src.run_transition_trials(200)
    # No jitter/starvation -> error is pure mix-block quantization, in [0, block).
    assert all(0 <= e.error_samples < cfg.mix_block_frames for e in events)


def test_mock_healthy_stack_has_zero_stem_drift():
    src = SampleClockModel(variance=VarianceModel(), seed=0)  # no skew, no noise
    for d in src.run_drift_pass(96):
        assert d.stem_lockstep_drift_samples == 0


def test_mock_is_not_measured():
    assert SampleClockModel().is_measured() is False


def test_mock_satisfies_timing_source_protocol():
    assert isinstance(SampleClockModel(), TimingSource)
    assert isinstance(GodotTimingSource(), TimingSource)


# --- determinism ---------------------------------------------------------
def test_same_seed_is_reproducible():
    a = SampleClockModel(variance=PROFILES["load"], seed=42).run_transition_trials(100)
    b = SampleClockModel(variance=PROFILES["load"], seed=42).run_transition_trials(100)
    assert [e.error_samples for e in a] == [e.error_samples for e in b]


def test_different_seed_varies():
    a = SampleClockModel(variance=PROFILES["load"], seed=1).run_transition_trials(100)
    b = SampleClockModel(variance=PROFILES["load"], seed=2).run_transition_trials(100)
    assert [e.error_samples for e in a] != [e.error_samples for e in b]


# --- gate evaluation (§3.1) ---------------------------------------------
def _run(profile: str, seed: int = 0):
    cfg = ClockConfig()
    src = SampleClockModel(cfg=cfg, variance=PROFILES[profile], seed=seed)
    return analyze(cfg, src.run_transition_trials(200), src.run_drift_pass(96))


def test_noload_profile_passes_gate():
    assert _run("noload").gate.verdict == "PASS"


def test_load_profile_passes_gate():
    # A healthy engine under load still meets §3.1.
    assert _run("load").gate.verdict == "PASS"


def test_failing_profile_fails_gate():
    a = _run("failing")
    assert a.gate.verdict == "FAIL"
    failed = {c.name for c in a.gate.criteria if not c.passed}
    # The broken engine trips transition, drift, and starvation criteria.
    assert "transition_error_p95_le_10ms" in failed
    assert "stem_drift_max_le_1ms" in failed
    assert "zero_starvation" in failed


def test_gate_thresholds_match_sad_3_1():
    assert TRANSITION_ERROR_MS == 10.0
    assert STEM_DRIFT_MS == 1.0


def test_analyze_flags_monotonic_shadow_drift():
    cfg = ClockConfig()
    var = VarianceModel(shadow_clock_drift_samples_per_bar=1.0)  # steady ramp
    src = SampleClockModel(cfg=cfg, variance=var, seed=0)
    a = analyze(cfg, src.run_transition_trials(50), src.run_drift_pass(96))
    trend_crit = next(c for c in a.gate.criteria if c.name == "no_monotonic_drift_trend")
    assert trend_crit.passed is False


# --- report round-trip ---------------------------------------------------
def test_summary_and_human_report_render(tmp_path):
    cfg = ClockConfig()
    src = SampleClockModel(cfg=cfg, variance=PROFILES["noload"], seed=0)
    a = analyze(cfg, src.run_transition_trials(50), src.run_drift_pass(64))
    summary = build_summary(cfg, {"measured": False, "source": "mock", "run": {}}, a)
    # JSON-serializable + carries the honesty flag.
    round_tripped = json.loads(json.dumps(summary))
    assert round_tripped["measured"] is False
    assert round_tripped["results"]["gate"]["verdict"] in ("PASS", "FAIL")
    text = human_summary(summary)
    assert "TD-11" in text
    assert "MODELLED" in text  # mock runs must announce they are not real audio


def test_godot_source_not_implemented_yet():
    # Until ZoneMusicPlayer (#144) lands, the real source refuses to fabricate.
    src = GodotTimingSource()
    with pytest.raises(NotImplementedError):
        src.run_transition_trials(1)
    with pytest.raises(NotImplementedError):
        src.run_drift_pass(1)
