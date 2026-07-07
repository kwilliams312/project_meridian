# SPDX-License-Identifier: Apache-2.0
"""TD-11 music-timing measurement harness (music SAD §3.1, issue #145).

Produces the reproducible TD-11 gate evidence: a sample-position
transition-accuracy distribution and a per-stem drift distribution for the
adaptive-music runtime (`ZoneMusicPlayer`), evaluated against the pass
thresholds in `docs/sad/music-sad.md` §3.1.

The harness is source-agnostic: it drives a `TimingSource` (the seam) and
computes statistics + a PASS/FAIL gate verdict from whatever timing samples the
source yields. Today the runnable source is `SampleClockModel` (a MODELLED mock,
clearly flagged in all output). When the real `ZoneMusicPlayer` lands (#144) a
`GodotTimingSource` implements the same protocol and the numbers become real —
nothing else in the harness changes.
"""

from .probe import (
    ClockConfig,
    DriftSample,
    GodotTimingSource,
    TimingSource,
    TransitionEvent,
)

__all__ = [
    "ClockConfig",
    "DriftSample",
    "GodotTimingSource",
    "TimingSource",
    "TransitionEvent",
]

__version__ = "0.1.0"
