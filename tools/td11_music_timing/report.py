# SPDX-License-Identifier: Apache-2.0
"""Output writers: summary.json + transitions.csv + drift.csv + human summary.

The CSVs are the "CSV artifact attached to the gate review" (music SAD §3.1);
summary.json is the machine-readable roll-up the #147 gate decision consumes;
the human summary is what a reviewer reads at a glance.
"""

from __future__ import annotations

import csv
import json
from pathlib import Path

from .probe import ClockConfig, DriftSample, TransitionEvent
from .stats import Analysis


def build_summary(
    cfg: ClockConfig,
    meta: dict,
    analysis: Analysis,
) -> dict:
    return {
        "harness": "td11_music_timing",
        "spec": "music SAD §3.1 (TD-11)",
        "issue": 145,
        "measured": meta.get("measured", False),
        "source": meta.get("source", "mock"),
        "note": meta.get("note", ""),
        "config": {
            "sample_rate": cfg.sample_rate,
            "bpm": cfg.bpm,
            "beats_per_bar": cfg.beats_per_bar,
            "mix_block_frames": cfg.mix_block_frames,
            "samples_per_bar": cfg.samples_per_bar,
        },
        "run": meta.get("run", {}),
        "results": analysis.to_dict(),
    }


def write_json(path: Path, summary: dict) -> None:
    path.write_text(json.dumps(summary, indent=2, sort_keys=False) + "\n")


def write_transitions_csv(path: Path, cfg: ClockConfig, events: list[TransitionEvent]) -> None:
    with path.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(
            [
                "index",
                "from_state",
                "to_state",
                "request_sample",
                "predicted_boundary_sample",
                "actual_switch_sample",
                "error_samples",
                "error_ms",
                "error_beats",
                "starved",
            ]
        )
        for e in events:
            w.writerow(
                [
                    e.index,
                    e.from_state,
                    e.to_state,
                    e.request_sample,
                    e.predicted_boundary_sample,
                    e.actual_switch_sample,
                    e.error_samples,
                    f"{cfg.samples_to_ms(e.error_samples):.4f}",
                    f"{cfg.samples_to_beats(e.error_samples):.5f}",
                    int(e.starved),
                ]
            )


def write_drift_csv(path: Path, cfg: ClockConfig, samples: list[DriftSample]) -> None:
    with path.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(
            [
                "bar_index",
                "shadow_clock_sample",
                "stem_lockstep_drift_samples",
                "stem_lockstep_drift_ms",
                "stack_vs_shadow_drift_samples",
                "stack_vs_shadow_drift_ms",
                "stem_positions",
            ]
        )
        for d in samples:
            w.writerow(
                [
                    d.bar_index,
                    d.shadow_clock_sample,
                    d.stem_lockstep_drift_samples,
                    f"{cfg.samples_to_ms(d.stem_lockstep_drift_samples):.5f}",
                    d.stack_vs_shadow_drift_samples,
                    f"{cfg.samples_to_ms(d.stack_vs_shadow_drift_samples):.5f}",
                    " ".join(str(p) for p in d.stem_positions),
                ]
            )


def _dist_line(label: str, dist) -> str:
    return (
        f"  {label:<22} min={dist.min:8.4f}  median={dist.median:8.4f}  "
        f"p95={dist.p95:8.4f}  max={dist.max:8.4f}  ({dist.unit}, n={dist.count})"
    )


def human_summary(summary: dict) -> str:
    r = summary["results"]
    cfg = summary["config"]
    lines: list[str] = []
    lines.append("=" * 74)
    lines.append("TD-11 MUSIC-TIMING MEASUREMENT HARNESS  (music SAD §3.1, issue #145)")
    lines.append("=" * 74)
    if not summary["measured"]:
        lines.append(
            "!! MODELLED numbers — source '%s' is a MOCK (ZoneMusicPlayer #144 "
            "not built yet)." % summary["source"]
        )
        lines.append("!! These are NOT measured from real audio. See README §Mock vs real.")
    else:
        lines.append("Measured against real audio (%s)." % summary["source"])
    run = summary.get("run", {})
    lines.append(
        "Config: %d Hz, %.1f BPM, %d/4, mix-block %d frames  |  %s"
        % (
            cfg["sample_rate"],
            cfg["bpm"],
            cfg["beats_per_bar"],
            cfg["mix_block_frames"],
            ", ".join(f"{k}={v}" for k, v in run.items()),
        )
    )
    lines.append("")
    lines.append("Transition accuracy (|actual − predicted boundary|):")

    def _d(key):
        from types import SimpleNamespace

        return SimpleNamespace(**r[key])

    lines.append(_dist_line("error", _d("transition_error_ms")))
    lines.append(_dist_line("error (beats)", _d("transition_error_beats")))
    lines.append(f"  worst case             {r['worst_case_bars']:.4f} bars")
    lines.append("")
    lines.append("Drift (over the loop pass):")
    lines.append(_dist_line("stem lockstep", _d("stem_drift_ms")))
    lines.append(_dist_line("stack vs shadow", _d("stack_vs_shadow_ms")))
    lines.append(
        f"  trend                  {r['stack_vs_shadow_trend_ms_per_bar']:+.5f} ms/bar"
    )
    lines.append(f"  starvation events      {r['starvation_count']}")
    lines.append("")
    lines.append("Gate criteria (music SAD §3.1):")
    for c in r["gate"]["criteria"]:
        mark = "PASS" if c["passed"] else "FAIL"
        lines.append(f"  [{mark}] {c['name']:<34} {c['detail']}")
    lines.append("")
    lines.append(f"VERDICT: {r['gate']['verdict']}")
    lines.append("=" * 74)
    return "\n".join(lines)
