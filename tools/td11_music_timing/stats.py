# SPDX-License-Identifier: Apache-2.0
"""Distributions + the TD-11 gate verdict (music SAD §3.1 pass thresholds).

Stdlib-only (no numpy) to stay inside the repo's tooling dependency footprint.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field

from .probe import ClockConfig, DriftSample, TransitionEvent

# --- pass thresholds, verbatim from music SAD §3.1 -----------------------
TRANSITION_ERROR_MS = 10.0  # |error| ≤ ±10 ms from the intended boundary
STEM_DRIFT_MS = 1.0  # stem lockstep drift ≤ 1 ms (≈48 samples) sustained
# "≤ 1 bar worst case including queueing" and "zero audio-thread starvation"
# and "no monotonic drift trend" are the remaining §3.1 criteria.


def percentile(values: list[float], pct: float) -> float:
    """Linear-interpolation percentile (numpy 'linear' method), stdlib-only."""
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(values)
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(rank)
    hi = min(lo + 1, len(ordered) - 1)
    frac = rank - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * frac


@dataclass
class Distribution:
    """min / median / p95 / max (+ mean, count) over a sample set."""

    unit: str
    count: int
    min: float
    median: float
    p95: float
    max: float
    mean: float

    @classmethod
    def of(cls, values: list[float], unit: str) -> "Distribution":
        if not values:
            return cls(unit=unit, count=0, min=0.0, median=0.0, p95=0.0, max=0.0, mean=0.0)
        return cls(
            unit=unit,
            count=len(values),
            min=min(values),
            median=percentile(values, 50.0),
            p95=percentile(values, 95.0),
            max=max(values),
            mean=sum(values) / len(values),
        )


def _slope(ys: list[float]) -> float:
    """Least-squares slope of ys over x = 0..n-1 (drift-trend detector)."""
    n = len(ys)
    if n < 2:
        return 0.0
    xs = list(range(n))
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den else 0.0


@dataclass
class Criterion:
    name: str
    passed: bool
    detail: str


@dataclass
class GateResult:
    verdict: str  # "PASS" | "FAIL"
    criteria: list[Criterion] = field(default_factory=list)

    def add(self, name: str, passed: bool, detail: str) -> None:
        self.criteria.append(Criterion(name, passed, detail))


@dataclass
class Analysis:
    """Everything the #147 gate review needs, computed from raw samples."""

    transition_error_ms: Distribution
    transition_error_beats: Distribution
    stem_drift_ms: Distribution
    stack_vs_shadow_ms: Distribution
    starvation_count: int
    stack_vs_shadow_trend_ms_per_bar: float
    worst_case_bars: float
    gate: GateResult

    def to_dict(self) -> dict:
        d = {
            "transition_error_ms": asdict(self.transition_error_ms),
            "transition_error_beats": asdict(self.transition_error_beats),
            "stem_drift_ms": asdict(self.stem_drift_ms),
            "stack_vs_shadow_ms": asdict(self.stack_vs_shadow_ms),
            "starvation_count": self.starvation_count,
            "stack_vs_shadow_trend_ms_per_bar": self.stack_vs_shadow_trend_ms_per_bar,
            "worst_case_bars": self.worst_case_bars,
            "gate": {
                "verdict": self.gate.verdict,
                "criteria": [asdict(c) for c in self.gate.criteria],
            },
        }
        return d


def analyze(
    cfg: ClockConfig,
    transitions: list[TransitionEvent],
    drift: list[DriftSample],
) -> Analysis:
    """Compute the §3.1 distributions and the PASS/FAIL gate verdict."""
    err_samples = [t.error_samples for t in transitions]
    err_ms = [cfg.samples_to_ms(e) for e in err_samples]
    err_abs_ms = [abs(x) for x in err_ms]
    err_beats = [abs(cfg.samples_to_beats(e)) for e in err_samples]
    worst_case_bars = (max(err_samples) / cfg.samples_per_bar) if err_samples else 0.0

    stem_drift_samples = [d.stem_lockstep_drift_samples for d in drift]
    stem_drift_ms = [cfg.samples_to_ms(s) for s in stem_drift_samples]
    stack_shadow_samples = [d.stack_vs_shadow_drift_samples for d in drift]
    stack_shadow_ms = [cfg.samples_to_ms(s) for s in stack_shadow_samples]
    trend = _slope(stack_shadow_ms)

    starvation = sum(1 for t in transitions if t.starved)

    t_err = Distribution.of(err_abs_ms, "ms")
    t_beats = Distribution.of(err_beats, "beats")
    d_drift = Distribution.of(stem_drift_ms, "ms")
    d_stack = Distribution.of([abs(x) for x in stack_shadow_ms], "ms")

    gate = GateResult(verdict="PASS")
    # §3.1: transition error ≤ ±10 ms (judged on p95) AND ≤ 1 bar worst case.
    gate.add(
        "transition_error_p95_le_10ms",
        t_err.p95 <= TRANSITION_ERROR_MS,
        f"p95={t_err.p95:.3f} ms (limit {TRANSITION_ERROR_MS} ms)",
    )
    gate.add(
        "transition_worst_case_le_1_bar",
        worst_case_bars <= 1.0,
        f"worst case {worst_case_bars:.4f} bars (limit 1 bar)",
    )
    # §3.1: stem lockstep drift ≤ 1 ms sustained (judged on max).
    gate.add(
        "stem_drift_max_le_1ms",
        d_drift.max <= STEM_DRIFT_MS,
        f"max={d_drift.max:.4f} ms (limit {STEM_DRIFT_MS} ms)",
    )
    # §3.1: no monotonic drift trend over the full pass.
    trend_ok = abs(trend) <= 0.02  # ms per bar; ~0 for a healthy stack
    gate.add(
        "no_monotonic_drift_trend",
        trend_ok,
        f"stack-vs-shadow trend {trend:+.5f} ms/bar (limit ±0.02)",
    )
    # §3.1: zero audio-thread starvation events.
    gate.add(
        "zero_starvation",
        starvation == 0,
        f"{starvation} starvation event(s) (limit 0)",
    )

    if not all(c.passed for c in gate.criteria):
        gate.verdict = "FAIL"

    return Analysis(
        transition_error_ms=t_err,
        transition_error_beats=t_beats,
        stem_drift_ms=d_drift,
        stack_vs_shadow_ms=d_stack,
        starvation_count=starvation,
        stack_vs_shadow_trend_ms_per_bar=trend,
        worst_case_bars=worst_case_bars,
        gate=gate,
    )
