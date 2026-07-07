# SPDX-License-Identifier: Apache-2.0
"""The `TimingSource` seam — where the real `ZoneMusicPlayer` (#144) plugs in.

TD-11 (music SAD §3.1) measures two quantities, both in the *sample* domain
(never wall clock):

  1. Transition accuracy — on every state flip, the sample error between the
     shadow bar clock's PREDICTED boundary and the sample position at which the
     interactive stream ACTUALLY switched (detected via the −60 dB→ramp edge on
     per-stem gain telemetry).  error = actual − predicted.

  2. Stem drift — per-stem playback positions across the `AudioStreamSynchronized`
     stack, sampled every bar over a full 64–128-bar pass; drift = max pairwise
     delta, plus the stack-vs-shadow-clock delta.

A `TimingSource` is anything that can yield those raw sample measurements. The
harness (`harness.py`) and statistics (`stats.py`) are written entirely against
this protocol, so the mock and the real Godot probe are interchangeable.

Ground-truth clock (§3.1), the value a real source MUST sample per mix step:

    ground_truth_sample =
          playback.get_playback_position()      * sample_rate
        + AudioServer.get_time_since_last_mix()  * sample_rate
        − AudioServer.get_output_latency()       * sample_rate

Wall clock is only recorded alongside to correlate with frame hitches; it is
never the pass/fail reference.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, runtime_checkable


@dataclass(frozen=True)
class ClockConfig:
    """Musical + audio-engine parameters that define the sample grid.

    Defaults mirror the illustrative stem sidecar in music SAD §4.2
    (bpm 96, 4/4) at Godot's default 44.1 kHz mix rate.
    """

    sample_rate: int = 44_100
    bpm: float = 96.0
    beats_per_bar: int = 4
    # Granularity at which the audio server advances stream playback (and thus
    # can apply an interactive transition) per mix step. This is the SINGLE most
    # sensitive modelling assumption in the mock and the exact quantity the real
    # probe (#144) pins empirically — see README §"The one number to pin".
    mix_block_frames: int = 128

    @property
    def samples_per_beat(self) -> float:
        return self.sample_rate * 60.0 / self.bpm

    @property
    def samples_per_bar(self) -> float:
        return self.samples_per_beat * self.beats_per_bar

    def samples_to_ms(self, samples: float) -> float:
        return samples * 1000.0 / self.sample_rate

    def samples_to_beats(self, samples: float) -> float:
        return samples / self.samples_per_beat

    def ms_to_samples(self, ms: float) -> float:
        return ms * self.sample_rate / 1000.0


@dataclass(frozen=True)
class TransitionEvent:
    """One state-flip measurement (the transition-accuracy probe).

    All sample fields are absolute stream sample positions.
    `error_samples = actual_switch_sample − predicted_boundary_sample`.
    A next-bar transition can only fire on or after the boundary, so a
    well-behaved source yields error_samples ≥ 0.
    """

    index: int
    request_sample: int  # sample position at which the flip was requested
    predicted_boundary_sample: int  # shadow bar clock's next-bar boundary
    actual_switch_sample: int  # where the gain-edge telemetry saw the switch
    from_state: str
    to_state: str
    starved: bool = False  # an audio-thread starvation event delayed this switch

    @property
    def error_samples(self) -> int:
        return self.actual_switch_sample - self.predicted_boundary_sample


@dataclass(frozen=True)
class DriftSample:
    """One per-bar drift measurement (the drift probe).

    `stem_positions` are the sampled playback positions (in samples) of every
    stem in the synchronized stack at bar `bar_index`. `shadow_clock_sample` is
    the shadow bar clock's expected position for that bar.
    """

    bar_index: int
    stem_positions: tuple[int, ...]
    shadow_clock_sample: int

    @property
    def stem_lockstep_drift_samples(self) -> int:
        """Max pairwise delta across the synchronized stack."""
        if not self.stem_positions:
            return 0
        return max(self.stem_positions) - min(self.stem_positions)

    @property
    def stack_vs_shadow_drift_samples(self) -> int:
        """Delta between the stack (median stem) and the shadow bar clock."""
        if not self.stem_positions:
            return 0
        ordered = sorted(self.stem_positions)
        median = ordered[len(ordered) // 2]
        return median - self.shadow_clock_sample


@runtime_checkable
class TimingSource(Protocol):
    """The seam. A real `ZoneMusicPlayer` probe implements exactly this."""

    def config(self) -> ClockConfig: ...

    def is_measured(self) -> bool:
        """False for a modelled/mock source, True when driving real audio."""
        ...

    def run_transition_trials(self, n: int) -> list[TransitionEvent]:
        """Script `n` state flips and return the measured transition events."""
        ...

    def run_drift_pass(self, bars: int) -> list[DriftSample]:
        """Sample per-stem + shadow-clock positions once per bar for `bars`."""
        ...


class GodotTimingSource:
    """Real-audio source — the plug-in point for `ZoneMusicPlayer` (#144).

    Not yet implemented: the M0 `ZoneMusicPlayer` runtime (music SAD §2.4,
    issue #144) and its per-stem gain telemetry do not exist yet. When they
    land, this source drives the real player over the debug state-switch channel
    and reads the §3.1 ground-truth sample clock + per-stem gain edges, returning
    the SAME `TransitionEvent` / `DriftSample` records the mock returns. The
    harness, statistics and report code need zero changes.

    Wiring checklist when #144 lands:
      * open the debug control channel to a running `ZoneMusicPlayer` (headless
        client or the bot-fleet load rig, #111);
      * per mix step, read
          playback.get_playback_position()*sr
          + AudioServer.get_time_since_last_mix()*sr
          − AudioServer.get_output_latency()*sr;
      * on each scripted flip, capture the predicted next-bar boundary from the
        shadow bar clock and the −60 dB→ramp gain edge sample as the actual
        switch; emit a `TransitionEvent`;
      * once per bar, read every stem's playback position + the shadow clock and
        emit a `DriftSample`.
    """

    def __init__(self, cfg: ClockConfig | None = None) -> None:
        self._cfg = cfg or ClockConfig()

    def config(self) -> ClockConfig:
        return self._cfg

    def is_measured(self) -> bool:
        return True

    def run_transition_trials(self, n: int) -> list[TransitionEvent]:
        raise NotImplementedError(
            "GodotTimingSource requires the ZoneMusicPlayer runtime (#144), "
            "which does not exist yet. Use SampleClockModel (--source mock) "
            "until #144 lands; see docstring for the wiring checklist."
        )

    def run_drift_pass(self, bars: int) -> list[DriftSample]:
        raise NotImplementedError(
            "GodotTimingSource requires the ZoneMusicPlayer runtime (#144), "
            "which does not exist yet. Use SampleClockModel (--source mock) "
            "until #144 lands; see docstring for the wiring checklist."
        )
