# SPDX-License-Identifier: Apache-2.0
"""`SampleClockModel` — the runnable MOCK `TimingSource`.

This models the *physics* of Godot's clip-level interactive-audio switching so
the harness produces a representative TD-11 number NOW, before the real
`ZoneMusicPlayer` exists (#144). Every number it emits is MODELLED, not measured
from real audio — the harness flags this everywhere (`is_measured() == False`).

The model is deliberately conservative and each variance source is a documented,
tunable knob so the mock's behaviour can be reasoned about and later validated
against the real probe:

Transition accuracy — actual = predicted + Σ delays, all ≥ 0 (a next-bar
transition can only fire on or after the boundary):

  * Mix-block quantization (structural floor). The state machine only polls
    "have we crossed the boundary?" once per audio mix step, so the switch lands
    on the first mix-block boundary at or after the predicted bar boundary:
    uniform[0, mix_block_frames) samples. This is the dominant error under no
    load and is irreducible without sample-accurate scheduling (exactly the
    property the GDExtension fallback, SAD §3.2, would add).
  * Scheduling jitter (CPU load). Callback lateness, modelled as a half-normal
    delay with std `sched_jitter_ms`.
  * Starvation tail (audio-thread underrun). With probability `starvation_prob`
    the switch slips by `starvation_blocks` extra mix blocks — the tail that
    moves p95/max and, per §3.1, is itself a hard fail if it ever occurs.

Stem drift — `AudioStreamSynchronized` shares one playback clock, so a healthy
stack drifts by 0 samples by construction; `stem_skew_samples_per_bar` injects a
failing implementation. The shadow bar clock is float-accumulated in script, so
`shadow_clock_drift_samples_per_bar` models the rounding the drift probe is meant
to catch.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass

from .probe import ClockConfig, DriftSample, TransitionEvent

_STATES = ("explore", "tension", "combat")


@dataclass(frozen=True)
class VarianceModel:
    """Tunable sources of timing variance. Defaults = M0 reference, no load."""

    sched_jitter_ms: float = 0.0
    starvation_prob: float = 0.0
    starvation_blocks: int = 8
    stem_count: int = 5  # 5–7 stems per zone set (music PRD §3)
    stem_skew_samples_per_bar: float = 0.0
    shadow_clock_drift_samples_per_bar: float = 0.0
    stem_measure_noise_samples: float = 0.0  # jitter in reading a stem position


# Named profiles the CLI exposes. "noload" is the clean M0 reference; "load"
# turns on the jitter + starvation the 50-bot-fleet gate run (#147) exercises.
PROFILES: dict[str, VarianceModel] = {
    "noload": VarianceModel(),
    "load": VarianceModel(
        sched_jitter_ms=1.5,
        starvation_prob=0.0,  # a healthy engine under load still starves 0 times
        stem_measure_noise_samples=2.0,
        shadow_clock_drift_samples_per_bar=0.0,
    ),
    # An intentionally BROKEN engine — proves the harness actually fails a bad
    # runtime (used by the tests and useful as a negative control at the gate).
    "failing": VarianceModel(
        sched_jitter_ms=6.0,
        starvation_prob=0.02,
        starvation_blocks=16,
        stem_skew_samples_per_bar=1.5,
        shadow_clock_drift_samples_per_bar=0.4,
        stem_measure_noise_samples=3.0,
    ),
}


class SampleClockModel:
    """A deterministic (seeded) mock `TimingSource`. `is_measured()` is False."""

    def __init__(
        self,
        cfg: ClockConfig | None = None,
        variance: VarianceModel | None = None,
        seed: int = 0,
    ) -> None:
        self._cfg = cfg or ClockConfig()
        self._var = variance or VarianceModel()
        self._rng = random.Random(seed)

    def config(self) -> ClockConfig:
        return self._cfg

    def is_measured(self) -> bool:
        return False

    # --- transition-accuracy probe ---------------------------------------
    def run_transition_trials(self, n: int) -> list[TransitionEvent]:
        cfg = self._cfg
        var = self._var
        spb = cfg.samples_per_bar
        block = cfg.mix_block_frames
        jitter_sigma = cfg.ms_to_samples(var.sched_jitter_ms)

        events: list[TransitionEvent] = []
        # Flips are scripted every 4–16 bars (§3.1). Walk a running bar cursor.
        bar_cursor = self._rng.uniform(0.0, 1.0)
        state = self._STATE(0)
        for i in range(n):
            bar_cursor += self._rng.randint(4, 16)
            # Request lands at an arbitrary offset within a bar.
            request_sample = int((bar_cursor + self._rng.random()) * spb)
            # Shadow bar clock: next bar boundary at/after the request.
            next_bar = math.ceil(request_sample / spb)
            predicted = int(round(next_bar * spb))

            # Structural floor: first mix-block boundary at/after predicted.
            actual = math.ceil(predicted / block) * block
            # CPU-load scheduling jitter (half-normal, never early).
            if jitter_sigma > 0.0:
                actual += int(abs(self._rng.gauss(0.0, jitter_sigma)))
            # Starvation tail.
            starved = False
            if var.starvation_prob > 0.0 and self._rng.random() < var.starvation_prob:
                actual += var.starvation_blocks * block
                starved = True

            nxt = self._STATE(i + 1)
            events.append(
                TransitionEvent(
                    index=i,
                    request_sample=request_sample,
                    predicted_boundary_sample=predicted,
                    actual_switch_sample=actual,
                    from_state=state,
                    to_state=nxt,
                    starved=starved,
                )
            )
            state = nxt
        return events

    # --- drift probe ------------------------------------------------------
    def run_drift_pass(self, bars: int) -> list[DriftSample]:
        cfg = self._cfg
        var = self._var
        spb = cfg.samples_per_bar

        samples: list[DriftSample] = []
        for bar in range(bars):
            nominal = int(round(bar * spb))
            positions: list[int] = []
            for stem in range(var.stem_count):
                # Per-stem linear skew (0 for a healthy synchronized stack) +
                # symmetric measurement noise.
                skew = var.stem_skew_samples_per_bar * bar * (stem - (var.stem_count - 1) / 2.0)
                noise = (
                    self._rng.gauss(0.0, var.stem_measure_noise_samples)
                    if var.stem_measure_noise_samples > 0.0
                    else 0.0
                )
                positions.append(int(round(nominal + skew + noise)))
            # Shadow bar clock: float accumulation drifts monotonically.
            shadow = int(round(nominal + var.shadow_clock_drift_samples_per_bar * bar))
            samples.append(
                DriftSample(
                    bar_index=bar,
                    stem_positions=tuple(positions),
                    shadow_clock_sample=shadow,
                )
            )
        return samples

    @staticmethod
    def _STATE(i: int) -> str:
        return _STATES[i % len(_STATES)]
