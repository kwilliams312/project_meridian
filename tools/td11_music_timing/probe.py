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

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Protocol, runtime_checkable


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


# The Godot project the probe script lives in, and the script's res:// path.
# Repo layout: <repo>/tools/td11_music_timing/probe.py, <repo>/client/project/.
_CLIENT_PROJECT = Path(__file__).resolve().parents[2] / "client" / "project"
_PROBE_SCRIPT = "res://audio/music_timing_probe.gd"
# stdout markers the probe frames its JSON with (music_timing_probe.gd), so we
# can slice the payload out of any engine banner noise.
_BEGIN = "@@TD11_PROBE_BEGIN@@"
_END = "@@TD11_PROBE_END@@"


def _default_godot_runner(args: list[str]) -> str:
    """Locate the Godot 4.7 binary and run the probe, returning its stdout.

    Honors $GODOT_BIN (same knob scripts/dev/run-client.sh uses), then falls
    back to `godot` / `godot4` on PATH. Raises a clear error when Godot is not
    installed — a `--source godot` run is only meaningful where the engine and
    the ZoneMusicPlayer runtime exist (the #147 bot-fleet rig, #111).
    """
    godot = os.environ.get("GODOT_BIN") or shutil.which("godot") or shutil.which("godot4")
    if not godot:
        raise RuntimeError(
            "GodotTimingSource needs a Godot 4.7 binary to drive the real "
            "ZoneMusicPlayer probe, but none was found. Set $GODOT_BIN or put "
            "`godot` on PATH (this is expected only on a machine with the engine, "
            "e.g. the #147 gate rig). Use `--source mock` for a modelled run."
        )
    cmd = [godot, "--headless", "--path", str(_CLIENT_PROJECT), "--script",
           _PROBE_SCRIPT, "--"] + args
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if proc.returncode != 0:
        raise RuntimeError(
            f"ZoneMusicPlayer probe exited {proc.returncode}:\n{proc.stderr[-2000:]}"
        )
    return proc.stdout


class GodotTimingSource:
    """Real-audio source — drives the real `ZoneMusicPlayer` (#144) probe.

    Implemented as of #144: shells out (via `runner`) to
    `res://audio/music_timing_probe.gd`, which runs a live ZoneMusicPlayer under
    `--headless`, scripts state flips, and prints the SAME `TransitionEvent` /
    `DriftSample` records the mock returns, as framed JSON. The harness, stats,
    and report code are unchanged — that is the seam.

    `is_measured()` is True: the numbers come from a real audio graph + the §3.1
    ground-truth sample clock, not a model. Headless (Dummy driver) still
    advances the mix, so timing is measured — but off the min-spec device / 50-bot
    load rig; the #147 gate re-runs this under that rig (#111) for the
    authoritative gate evidence.

    `runner(args)` is injectable so parsing is unit-testable without Godot; the
    default locates the engine and launches the probe.
    """

    def __init__(
        self,
        cfg: ClockConfig | None = None,
        runner: Callable[[list[str]], str] | None = None,
    ) -> None:
        self._cfg = cfg or ClockConfig()
        self._runner = runner or _default_godot_runner

    def config(self) -> ClockConfig:
        return self._cfg

    def is_measured(self) -> bool:
        return True

    def _probe_args(self, trials: int, bars: int) -> list[str]:
        c = self._cfg
        return [
            "--trials", str(trials), "--bars", str(bars),
            "--bpm", str(c.bpm), "--beats-per-bar", str(c.beats_per_bar),
            "--sample-rate", str(c.sample_rate),
        ]

    def _run_probe(self, trials: int, bars: int) -> dict:
        raw = self._runner(self._probe_args(trials, bars))
        return _parse_probe_json(raw)

    def run_transition_trials(self, n: int) -> list[TransitionEvent]:
        payload = self._run_probe(trials=n, bars=0)
        return [_transition_from_dict(d) for d in payload.get("transitions", [])]

    def run_drift_pass(self, bars: int) -> list[DriftSample]:
        payload = self._run_probe(trials=0, bars=bars)
        return [_drift_from_dict(d) for d in payload.get("drift", [])]


def _parse_probe_json(stdout: str) -> dict:
    """Slice the framed JSON payload out of the probe's stdout and parse it."""
    if _BEGIN not in stdout or _END not in stdout:
        raise RuntimeError(
            "ZoneMusicPlayer probe produced no framed JSON payload "
            f"(missing {_BEGIN}/{_END} markers). Raw tail:\n{stdout[-1000:]}"
        )
    body = stdout.split(_BEGIN, 1)[1].split(_END, 1)[0].strip()
    return json.loads(body)


def _transition_from_dict(d: dict) -> TransitionEvent:
    return TransitionEvent(
        index=int(d["index"]),
        request_sample=int(d["request_sample"]),
        predicted_boundary_sample=int(d["predicted_boundary_sample"]),
        actual_switch_sample=int(d["actual_switch_sample"]),
        from_state=str(d["from_state"]),
        to_state=str(d["to_state"]),
        starved=bool(d.get("starved", False)),
    )


def _drift_from_dict(d: dict) -> DriftSample:
    return DriftSample(
        bar_index=int(d["bar_index"]),
        stem_positions=tuple(int(x) for x in d["stem_positions"]),
        shadow_clock_sample=int(d["shadow_clock_sample"]),
    )
