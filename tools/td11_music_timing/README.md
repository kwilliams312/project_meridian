<!-- SPDX-License-Identifier: Apache-2.0 -->
# TD-11 music-timing measurement harness (issue #145)

The measurement tool for the **TD-11 decision gate** (music SAD
[§3.1](../../docs/sad/music-sad.md)): does Godot's native interactive audio
(`AudioStreamInteractive` + `AudioStreamSynchronized`) hold **bar-accurate
transitions with no audible drift under load**, or do we build the custom
GDExtension stem mixer (SAD §3.2)?

This harness produces the **numbers**. It does not make the decision — that is
issue **#147**, which runs this harness under the 50-bot-fleet load rig (#111)
and records the ruling. The harness ships permanently and reruns each milestone
as a regression gate.

## What TD-11 measures (and our interpretation)

TD-11's definition is unambiguous in music SAD §3.1 — two sample-domain probes:

1. **Transition accuracy.** On every state flip (explore→tension→combat), the
   sample error between the shadow bar clock's **predicted** next-bar boundary
   and the sample position where the interactive stream **actually** switched
   (detected via the −60 dB→ramp edge on per-stem gain telemetry).
   `error = actual − predicted`, reported in ms and in beats.

2. **Stem drift.** Per-stem playback positions across the `AudioStreamSynchronized`
   stack, sampled once per bar over a full 64–128-bar pass. `stem lockstep drift`
   = max pairwise delta; `stack-vs-shadow` drift = stack vs the bar clock (catches
   whole-stack drift against the musical grid after repeated transitions).

Everything is measured in the **sample domain, never wall clock** (§3.1). The
ground-truth clock a real source samples per mix step is:

```
ground_truth_sample =  playback.get_playback_position()      * sample_rate
                     +  AudioServer.get_time_since_last_mix() * sample_rate
                     −  AudioServer.get_output_latency()      * sample_rate
```

### Pass thresholds (music SAD §3.1)

| Criterion | Threshold | Judged on |
|-----------|-----------|-----------|
| Transition error | ≤ ±10 ms from the intended boundary | p95 |
| Transition worst case | ≤ 1 bar (incl. queueing) | max |
| Stem lockstep drift | ≤ 1 ms (≈48 samples) sustained | max |
| Monotonic drift trend | none over the full pass | least-squares slope |
| Audio-thread starvation | zero events | count |

Any threshold failed under load ⇒ the gate picks the GDExtension fallback.

## How to run

```bash
# From the repo root. Exit code is 0 on gate PASS, 1 on FAIL.
PYTHONPATH=tools python3 -m td11_music_timing --profile noload

# Full artifact set (summary.json + transitions.csv + drift.csv) for the review:
PYTHONPATH=tools python3 -m td11_music_timing --profile load --out out/td11

# Options
#   --source {mock,godot}   mock (runnable now) | real ZoneMusicPlayer probe (#144)
#   --profile {noload,load,failing}   mock variance profile
#   --trials N              scripted state flips        (default 200)
#   --bars N                drift-pass length, 64–128   (default 96)
#   --seed N                RNG seed (determinism)      (default 0)
#   --bpm / --beats-per-bar / --sample-rate / --mix-block
#   --out DIR               write the CSV + JSON artifacts
```

Outputs:
- **`summary.json`** — machine-readable roll-up (config, distributions, per-criterion
  gate result, verdict). What #147 consumes.
- **`transitions.csv`** — one row per flip (predicted/actual samples, error in
  samples/ms/beats, starvation flag). The §3.1 CSV artifact.
- **`drift.csv`** — one row per bar (per-stem positions, lockstep + stack-vs-shadow drift).
- **human summary** on stdout — min / median / p95 / max per probe + PASS/FAIL.

## How to read the result

- **`transition_error_ms` p95** is the headline transition number; it must be
  ≤ 10 ms. `max` must also be ≤ 1 bar.
- **`stem_drift_ms` max** must be ≤ 1 ms and **must not trend** — a rising
  `stack_vs_shadow` slope means the whole stack is walking off the grid.
- **`starvation_count`** must be 0.
- **`gate.verdict`** is the single PASS/FAIL, mirrored by the process exit code.

## Mock vs real — the seam

`ZoneMusicPlayer` (#144) does not exist yet, so the harness ships with a
**modelled** source. Every mock run announces itself (`measured: false`, a
`MODELLED` banner) — the numbers are representative, **not measured from real
audio**.

- **`SampleClockModel`** (`mock_source.py`) — the runnable source today. It
  models the physics of clip-level, mix-step-quantized switching with documented,
  tunable variance knobs (`VarianceModel`). Profiles: `noload` (clean M0
  reference), `load` (a healthy engine under CPU load — jitter + read noise),
  `failing` (an intentionally broken engine — a negative control proving the
  harness fails a bad runtime).
- **`GodotTimingSource`** (`probe.py`) — **the plug-in point.** It implements the
  same `TimingSource` protocol and returns the same `TransitionEvent` /
  `DriftSample` records. When #144 lands, wire it to the real player's debug
  state-switch channel + per-stem gain telemetry (checklist in its docstring);
  the harness, statistics, and report code change **not at all**. Then run
  `--source godot` under the #111 bot fleet for the #147 gate evidence.

### The one number to pin

The mock's dominant transition-error term is **`mix_block_frames`** — the
granularity at which the audio server advances stream playback (and can apply a
transition) per mix step. The default (128 frames) is representative; the true
value — and whether Godot's `AudioStreamInteractive` schedules the crossfade
sample-accurately *within* a mix step or only at its boundary — is exactly what
the gate is meant to determine empirically. The real `GodotTimingSource` run
resolves it; treat the mock's absolute transition numbers as an order-of-magnitude
placeholder, not a verdict on native Godot audio.

## Sources of variance (reproducibility)

The harness is deterministic for a fixed `--seed`: same seed ⇒ byte-identical
`summary.json`. Different seeds model real run-to-run variance (scheduling
jitter, starvation incidence). For the gate, run several seeds (and, with the
real source, several load passes) and report the distribution across runs, not a
single lucky seed. Under the real source, additional non-modelled variance comes
from OS audio-callback scheduling, GPU/CPU contention on min-spec, and
pack-streaming activity on the bot fleet.

## Tests

`tests/test_td11_music_timing.py` (repo pytest suite): sample math, mock physics
invariants (error ≥ 0, quantization bounded by the mix block, zero drift for a
healthy stack), §3.1 gate evaluation on all three profiles, determinism, and the
report round-trip.

```bash
uv run pytest tests/test_td11_music_timing.py -q
```
