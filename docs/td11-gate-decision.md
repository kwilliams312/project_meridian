<!-- SPDX-License-Identifier: Apache-2.0 -->
# TD-11 gate — under-load run + native-vs-fallback decision (issue #147)

**Decision record for the TD-11 adaptive-music timing gate** (epic
[#15](https://github.com/kwilliams312/project_meridian/issues/15), music SAD
[§3.1](sad/music-sad.md)). It runs the TD-11 measurement harness
(`tools/td11_music_timing/`, shipped by #145, real-source seam by #144) under
load and records the ruling that #147 asks for: **native Godot audio
(`AudioStreamInteractive` + `AudioStreamSynchronized`) vs a custom GDExtension
stem mixer (SAD §3.2)** for adaptive music.

The binding one-line ruling is folded into the sync log as **D-36**
([§15, docs/01-SYNC-DECISIONS.md](01-SYNC-DECISIONS.md)). This document is the
evidence it reads against — the terrain-eval.md analogue for the music track.

---

## 1. Verdict

| | |
|---|---|
| **Modelled under-load gate** | **PASS** — all five §3.1 criteria, 10/10 seeds, wide margins (§4). |
| **Negative control** (`--profile failing`, same load params) | **FAIL** (exit 1) — the gate is not rubber-stamping (§5). |
| **Provisional ruling** | **Proceed on NATIVE Godot audio**; hold the GDExtension mixer (SAD §3.2) in reserve. |
| **Authoritative gate** | **Owner-runtime, pending** — the measured `--source godot` pass under the #111 bot fleet on min-spec still has to run to *ratify* the ruling (§6). |

**Honesty note.** Every number below is **MODELLED**, not measured from real
audio — the harness stamps `measured: false` and a `MODELLED` banner on every
mock run. The model is a conservative physics stand-in for clip-level,
mix-step-quantized switching (`SampleClockModel`); its absolute transition
numbers are an order-of-magnitude placeholder, **not** a verdict on native Godot
audio. What the modelled under-load pass establishes is that **nothing in the
modelled physics contradicts native audio**, and it exercises the exact
statistics/gate code the real probe will feed. The single number the model
cannot pin — whether `AudioStreamInteractive` schedules the crossfade
sample-accurately *within* a mix block or only at its boundary — is exactly what
the owner-runtime `--source godot` pass resolves. The ruling is therefore
**provisional (recommended native)** and is ratified only by the measured pass.

---

## 2. What "under load" means here (music SAD §3.1)

§3.1 defines the M0 load rig as a **50-connection bot fleet (bot client v0, #111)
on the flat bootstrap test map**, on the GTX 1060 / 16 GB reference machine, with
pack-streaming activity simulated, **state flips scripted every 4–16 bars for
≥30 min**. (The 50-client canned-replay scene is an M1 artifact per the Music SAD
v0.2 correction.)

The real bot fleet (#111) and adaptive track (#146) do not exist yet and there is
no reliable Godot binary in CI (#283), so this run reproduces the **load
*profile* and the ≥30-min scripted-flip timeline** against the modelled source,
which is CI-runnable anywhere:

- **`--profile load`** — the variance model documented as "the jitter + starvation
  the 50-bot-fleet gate run (#147) exercises" (CPU-load scheduling jitter +
  per-stem read noise on a healthy engine).
- **`--trials 2000`** — 2000 scripted state flips = **≈833 min of scripted
  timeline** at 96 BPM / 4-4 (each flip walks the bar cursor 4–16 bars ≈ 25 s),
  ~28× the §3.1 ≥30-min floor.
- **`--bars 128`** — the maximum §3.1 drift-pass length (64–128 bars).
- **10 seeds (0–9)** — the README's guidance is to report the distribution across
  seeds, not a single lucky seed.

---

## 3. Reproduce

Exact commands (from repo root; exit 0 = gate PASS, exit 1 = FAIL):

```bash
# Canonical under-load run — writes summary.json + transitions.csv + drift.csv
PYTHONPATH=tools python3 -m td11_music_timing \
    --source mock --profile load --trials 2000 --bars 128 --seed 0 \
    --out docs/reviews/td11-gate-under-load

# Cross-seed distribution (the numbers in §4)
for s in 0 1 2 3 4 5 6 7 8 9; do
  PYTHONPATH=tools python3 -m td11_music_timing \
      --source mock --profile load --trials 2000 --bars 128 --seed "$s" --quiet
done

# Negative control — same load params, an intentionally broken engine (must FAIL)
PYTHONPATH=tools python3 -m td11_music_timing \
    --source mock --profile failing --trials 2000 --bars 128 --seed 0
```

The harness is deterministic for a fixed seed: the same command yields a
byte-identical `summary.json`. The committed canonical artifacts (seed 0) live in
[`docs/reviews/td11-gate-under-load/`](reviews/td11-gate-under-load/) —
`summary.json` (machine roll-up), `transitions.csv` (one row per flip, the §3.1
CSV artifact), `drift.csv` (one row per bar).

**Config:** 44100 Hz, 96.0 BPM, 4/4, mix-block 128 frames (`ClockConfig`
defaults, mirroring the illustrative stem sidecar in music SAD §4.2).

**Pass thresholds (music SAD §3.1):**

| Criterion | Threshold | Judged on |
|-----------|-----------|-----------|
| Transition error | ≤ ±10 ms | p95 |
| Transition worst case | ≤ 1 bar (incl. queueing) | max |
| Stem lockstep drift | ≤ 1 ms (≈48 samples) sustained | max |
| Monotonic drift trend | none | least-squares slope (±0.02 ms/bar) |
| Audio-thread starvation | zero events | count |

---

## 4. Result — canonical run (seed 0) + cross-seed distribution

Canonical `--profile load --trials 2000 --bars 128 --seed 0`, verbatim harness
output:

```
==========================================================================
TD-11 MUSIC-TIMING MEASUREMENT HARNESS  (music SAD §3.1, issue #145)
==========================================================================
!! MODELLED numbers — source 'mock' is a MOCK (ZoneMusicPlayer #144 not built yet).
!! These are NOT measured from real audio. See README §Mock vs real.
Config: 44100 Hz, 96.0 BPM, 4/4, mix-block 128 frames  |  trials=2000, bars=128, seed=0, profile=load

Transition accuracy (|actual − predicted boundary|):
  error                  min=  0.0000  median=  2.5510  p95=  4.6485  max=  7.3923  (ms, n=2000)
  error (beats)          min=  0.0000  median=  0.0041  p95=  0.0074  max=  0.0118  (beats, n=2000)
  worst case             0.0030 bars

Drift (over the loop pass):
  stem lockstep          min=  0.0227  median=  0.1134  p95=  0.1814  max=  0.2041  (ms, n=128)
  stack vs shadow        min=  0.0000  median=  0.0227  p95=  0.0454  max=  0.0680  (ms, n=128)
  trend                  +0.00010 ms/bar
  starvation events      0

Gate criteria (music SAD §3.1):
  [PASS] transition_error_p95_le_10ms       p95=4.649 ms (limit 10.0 ms)
  [PASS] transition_worst_case_le_1_bar     worst case 0.0030 bars (limit 1 bar)
  [PASS] stem_drift_max_le_1ms              max=0.2041 ms (limit 1.0 ms)
  [PASS] no_monotonic_drift_trend           stack-vs-shadow trend +0.00010 ms/bar (limit ±0.02)
  [PASS] zero_starvation                    0 starvation event(s) (limit 0)

VERDICT: PASS
==========================================================================
```

Cross-seed distribution (10 seeds, same params):

| seed | p95 err (ms) | max err (ms) | worst (bars) | stem drift max (ms) | trend (ms/bar) | starv | verdict |
|------|-------------|-------------|--------------|---------------------|----------------|-------|---------|
| 0 | 4.6485 | 7.3923 | 0.0030 | 0.2041 | +0.00010 | 0 | PASS |
| 1 | 4.8991 | 7.5964 | 0.0030 | 0.1814 | +0.00012 | 0 | PASS |
| 2 | 4.7846 | 6.9161 | 0.0028 | 0.2041 | +0.00002 | 0 | PASS |
| 3 | 4.6032 | 6.9161 | 0.0028 | 0.2041 | +0.00001 | 0 | PASS |
| 4 | 4.7392 | 6.7120 | 0.0027 | 0.2041 | −0.00001 | 0 | PASS |
| 5 | 4.6712 | 7.4603 | 0.0030 | 0.2041 | +0.00005 | 0 | PASS |
| 6 | 4.7857 | 7.3469 | 0.0029 | 0.2041 | +0.00005 | 0 | PASS |
| 7 | 4.8084 | 7.3696 | 0.0029 | 0.2948 | +0.00007 | 0 | PASS |
| 8 | 4.7404 | 6.8254 | 0.0027 | 0.2268 | +0.00004 | 0 | PASS |
| 9 | 4.8980 | 7.3469 | 0.0029 | 0.2041 | +0.00005 | 0 | PASS |

**Margins.** Transition p95 sits at **~4.6–4.9 ms against the 10 ms limit** (≈2×
headroom); worst-case error is **~0.003 bars against the 1-bar limit** (>300×
headroom); stem lockstep drift peaks at **~0.20–0.29 ms against the 1 ms limit**;
the drift trend is **~0 ms/bar** (limit ±0.02); **zero** starvation on every seed.
The gate PASSES on all 10 seeds with no criterion close to its threshold.

---

## 5. Negative control (harness integrity)

The same load parameters against `--profile failing` (an intentionally broken
engine) must FAIL — otherwise a PASS proves nothing:

```
Gate criteria (music SAD §3.1):
  [FAIL] transition_error_p95_le_10ms       p95=14.558 ms (limit 10.0 ms)
  [PASS] transition_worst_case_le_1_bar     worst case 0.0242 bars (limit 1 bar)
  [FAIL] stem_drift_max_le_1ms              max=17.3243 ms (limit 1.0 ms)
  [PASS] no_monotonic_drift_trend           stack-vs-shadow trend -0.00899 ms/bar (limit ±0.02)
  [FAIL] zero_starvation                    42 starvation event(s) (limit 0)

VERDICT: FAIL
```

Process exit code **1**. The under-load gate discriminates a healthy runtime from
a broken one — the `load`-profile PASS is meaningful, not a rubber stamp.

---

## 6. Owner-runtime step (still required to ratify)

**Not run here** — deliberately. The authoritative TD-11 evidence per §3.1 is the
**measured** real-audio pass, which needs hardware CI does not have:

1. **A Godot 4.7 binary** driving the real `ZoneMusicPlayer` probe
   (`client/project/audio/music_timing_probe.gd`) via `--source godot`. None is
   present in this environment (`godot` not on PATH, `$GODOT_BIN` unset) — the
   harness raises a clear, actionable error and exits 1, confirming the seam is
   wired and simply gated on the engine (the #283 caveat).
2. **The #111 bot fleet** (50 connections on the flat bootstrap map, min-spec,
   pack-streaming simulated) and **an adaptive track to exercise** (#146).

When those exist, the owner runs, on the min-spec rig, the same command with
`--source godot` (it drives real audio; `measured: true`):

```bash
# On the #147 gate rig (min-spec + 50-bot fleet running), Godot 4.7 on PATH or $GODOT_BIN:
PYTHONPATH=tools python3 -m td11_music_timing \
    --source godot --trials 2000 --bars 128 \
    --out docs/reviews/td11-gate-under-load/godot
```

If that measured pass also clears all five §3.1 criteria, **D-36 is ratified as
final (native)**. If any criterion fails under real load, the gate selects the
**GDExtension stem mixer (SAD §3.2)** — a ~4–6-week, one-engineer swap of the
playback layer only; everything above the `ZoneMusicPlayer` API (state machine,
MusicDirector, assets, region data, sidecars) survives unchanged. Record the
outcome by updating D-36 with the measured numbers.

---

## 7. Files

- Harness: [`tools/td11_music_timing/`](../tools/td11_music_timing/) (README, CLI,
  mock + Godot sources, stats, report).
- Tests: `tests/test_td11_music_timing.py` (23 tests; full suite green).
- Canonical artifacts (seed 0): [`docs/reviews/td11-gate-under-load/`](reviews/td11-gate-under-load/).
- Sync-log ruling: [D-36 / §15](01-SYNC-DECISIONS.md).
