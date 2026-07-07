# SPDX-License-Identifier: Apache-2.0
"""Sample-domain bar/beat clock — the quantization math the ZoneMusicPlayer
shadow bar clock uses (music SAD §2.1.1) and the TD-11 harness measures against
(§3.1).

Everything is in the *sample* domain, never wall clock (SAD §3.1). This module
reuses `ClockConfig` from the TD-11 harness so the runtime, the reference core
here, and the measurement harness all agree on `samples_per_bar` /
`samples_per_beat` to the sample — there is exactly one sample-grid definition in
the tree.

A next-boundary transition can only fire on or after the request, so every
predicted boundary is >= the request sample. The formula mirrors the harness's
`predicted_boundary_sample` (`td11_music_timing.mock_source`) exactly:

    next_bar   = ceil(request / samples_per_bar)
    predicted  = round(next_bar * samples_per_bar)

so a `GodotTimingSource` reading a real ZoneMusicPlayer and this reference core
compute the identical predicted boundary for the same request.
"""

from __future__ import annotations

import math

# Reuse the single sample-grid definition owned by the TD-11 harness.
from td11_music_timing.probe import ClockConfig

# Quantization kinds a transition rule can request (music SAD §2.1 table).
QUANTIZE_BAR = "bar"
QUANTIZE_BEAT = "beat"
QUANTIZE_LOOP_END = "loop_end"
QUANTIZE_IMMEDIATE = "immediate"

_QUANTIZE_KINDS = frozenset(
    {QUANTIZE_BAR, QUANTIZE_BEAT, QUANTIZE_LOOP_END, QUANTIZE_IMMEDIATE}
)


def next_bar_sample(request_sample: int, cfg: ClockConfig) -> int:
    """First bar boundary at or after `request_sample` (music SAD §2.1.1)."""
    spb = cfg.samples_per_bar
    next_bar = math.ceil(request_sample / spb)
    return int(round(next_bar * spb))


def next_beat_sample(request_sample: int, cfg: ClockConfig) -> int:
    """First beat boundary at or after `request_sample` (combat entry, §2.2)."""
    spbeat = cfg.samples_per_beat
    next_beat = math.ceil(request_sample / spbeat)
    return int(round(next_beat * spbeat))


def next_loop_sample(request_sample: int, cfg: ClockConfig, length_bars: int) -> int:
    """First loop boundary at or after `request_sample` (silence scheduling)."""
    loop = cfg.samples_per_bar * length_bars
    next_loop = math.ceil(request_sample / loop)
    return int(round(next_loop * loop))


def boundary_sample(
    request_sample: int,
    cfg: ClockConfig,
    quantize: str,
    length_bars: int | None = None,
) -> int:
    """Predicted boundary sample for `quantize` — the shadow bar clock's answer.

    This is the `predicted_boundary_sample` a TD-11 `TransitionEvent` carries.
    """
    if quantize not in _QUANTIZE_KINDS:
        raise ValueError(f"unknown quantize kind: {quantize!r}")
    if quantize == QUANTIZE_IMMEDIATE:
        return int(request_sample)
    if quantize == QUANTIZE_BEAT:
        return next_beat_sample(request_sample, cfg)
    if quantize == QUANTIZE_LOOP_END:
        if length_bars is None:
            raise ValueError("loop_end quantize requires length_bars")
        return next_loop_sample(request_sample, cfg, length_bars)
    return next_bar_sample(request_sample, cfg)
