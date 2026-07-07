# SPDX-License-Identifier: Apache-2.0
"""CLI for the TD-11 music-timing measurement harness (issue #145).

    PYTHONPATH=tools python3 -m td11_music_timing [options]

Exit code 0 if the gate verdict is PASS, 1 if FAIL — so CI / the #147 gate run
can assert on it directly.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .mock_source import PROFILES, SampleClockModel, VarianceModel
from .probe import ClockConfig, GodotTimingSource, TimingSource
from .report import (
    build_summary,
    human_summary,
    write_drift_csv,
    write_json,
    write_transitions_csv,
)
from .stats import analyze


def _build_source(args) -> tuple[TimingSource, dict]:
    cfg = ClockConfig(
        sample_rate=args.sample_rate,
        bpm=args.bpm,
        beats_per_bar=args.beats_per_bar,
        mix_block_frames=args.mix_block,
    )
    if args.source == "godot":
        return GodotTimingSource(cfg), {
            "measured": True,
            "source": "godot",
            "note": "real ZoneMusicPlayer probe (#144)",
        }
    variance: VarianceModel = PROFILES[args.profile]
    src = SampleClockModel(cfg=cfg, variance=variance, seed=args.seed)
    return src, {
        "measured": False,
        "source": "mock",
        "note": (
            "MODELLED by SampleClockModel — profile '%s', seed %d. Not real "
            "audio; ZoneMusicPlayer (#144) not built yet." % (args.profile, args.seed)
        ),
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="td11_music_timing",
        description="TD-11 transition-accuracy + drift measurement harness "
        "(music SAD §3.1, issue #145).",
    )
    p.add_argument("--source", choices=["mock", "godot"], default="mock",
                   help="mock = SampleClockModel (runnable now); "
                   "godot = real ZoneMusicPlayer probe (#144, not built yet)")
    p.add_argument("--profile", choices=sorted(PROFILES), default="noload",
                   help="mock variance profile (default: noload)")
    p.add_argument("--trials", type=int, default=200,
                   help="number of scripted state flips (default: 200)")
    p.add_argument("--bars", type=int, default=96,
                   help="drift-pass length in bars, 64–128 per §3.1 (default: 96)")
    p.add_argument("--seed", type=int, default=0, help="RNG seed (default: 0)")
    p.add_argument("--sample-rate", type=int, default=44_100)
    p.add_argument("--bpm", type=float, default=96.0)
    p.add_argument("--beats-per-bar", type=int, default=4)
    p.add_argument("--mix-block", type=int, default=128,
                   help="mix-step granularity in frames — the dominant "
                   "quantization floor (default: 128; see README)")
    p.add_argument("--out", type=Path, default=None,
                   help="directory for summary.json, transitions.csv, drift.csv")
    p.add_argument("--quiet", action="store_true", help="suppress the human summary")
    args = p.parse_args(argv)

    source, meta = _build_source(args)
    cfg = source.config()

    transitions = source.run_transition_trials(args.trials)
    drift = source.run_drift_pass(args.bars)
    analysis = analyze(cfg, transitions, drift)

    meta["run"] = {"trials": args.trials, "bars": args.bars, "seed": args.seed,
                   "profile": args.profile if args.source == "mock" else "-"}
    summary = build_summary(cfg, meta, analysis)

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        write_json(args.out / "summary.json", summary)
        write_transitions_csv(args.out / "transitions.csv", cfg, transitions)
        write_drift_csv(args.out / "drift.csv", cfg, drift)

    if not args.quiet:
        print(human_summary(summary))
        if args.out:
            print("\nArtifacts written to %s/ (summary.json, transitions.csv, drift.csv)"
                  % args.out)

    return 0 if analysis.gate.verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
