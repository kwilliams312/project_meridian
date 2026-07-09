# Design — Offline Strudel Stem-Rendering Pipeline

**Date:** 2026-07-08
**Track:** Music / Tools
**Status:** Draft (design approved in brainstorming; pending spec review → implementation plan)
**Relates to:** Music PRD [AUD-02 §2](../../prd/music-prd.md), `meridian/asset@1`
([asset.schema.yaml](../../../schema/content/asset.schema.yaml)), TD-09 provenance,
[CONTRIBUTING.md](../../../CONTRIBUTING.md) clean-room / asset-provenance policy.

---

## 1. Summary

Let developers and musicians author adaptive-music **stems as [Strudel](https://strudel.cc)
patterns** (`.strudel` code) and render them **offline, deterministically, in CI** into the
WAV stems the existing `music_stem` asset contract already expects. The rendered audio flows
through the unchanged content pipeline (TLS-01 encode → Ogg → `.mcpack` → `ZoneMusicPlayer`).

**The core architectural fact that makes this safe:** Strudel splits into a pure-JS *pattern
engine* (`@strudel/core` — `queryArc()` returns timed events, no audio) and a browser Web-Audio
*sound engine* (`superdough`, wrapped by `@strudel/webaudio`). We use both — but **only inside a
build-time Node tool that is never shipped.** No Strudel, no JS runtime, and no AGPL-licensed
code touches the client or server distributables.

### 1.1 Why this shape (decision trail)

The original interest was *runtime* generativity (music that never loops identically, reacting
to gameplay more granularly than crossfading pre-baked stems). Evaluated three runtime options:

| Option | Verdict |
|--------|---------|
| A — headless browser (CEF) running full Strudel, capture audio | Rejected: ~150 MB, second audio clock (drift/latency), can't route native buses/samples, min-spec + packaging pain, AGPL in the shipped client. |
| B — embed `@strudel/core` in QuickJS + native Godot audio backend | Technically strong, but **AGPL-3.0** forces the whole client copyleft and is one-directionally incompatible with the project's permissive stance. |
| **C — Strudel as an offline authoring tool; ship rendered audio only** | **Chosen.** No AGPL exposure, no client/server runtime risk. |

**Accepted trade-off:** runtime infinite-variation is given up. It is recovered *offline* as a
deep library of deterministic variants (§5), shuffled at runtime by the existing AUD-02 machinery
(silence scheduling + randomized section re-entry, Music PRD §2.2). "Never repeats" becomes
"a large, cheaply-produced, regenerable variant pool" — an acceptable trade for an MMO.

## 2. Goals / non-goals

**Goals**
- Author adaptive-music stems as code — no DAW / audio-tool requirement for developers.
- Deterministic, reproducible renders (same source ⇒ same audio) regenerable in CI.
- Output conforms to the existing `music_stem` metadata contract with **zero client/server change**.
- Provenance and license correctness enforced automatically (TD-09, CONTRIBUTING.md).

**Non-goals**
- No runtime music generation; nothing new runs in client or server.
- No player-facing music tooling.
- This pipeline targets **AUD-02 adaptive stem sets only.** Plain `.mp3/.wav` file playback
  remains a separate, pre-existing client capability and is not an *output* of this pipeline.
- Ambient beds (`AmbienceBed`, PRD §4) and stingers are out of initial scope (natural follow-ups
  — the mechanism generalizes, but the contract/validation differs).

## 3. Where it lives — the AGPL firewall

A new build-time tool, proposed at `tools/strudel_render/` (Node), sitting beside the existing
music tooling (`tools/zone_music`, `tools/td11_music_timing`).

- The tool depends on vendored/pinned `@strudel/core`, `@strudel/mini`, `@strudel/transpiler`,
  `@strudel/tonal` (pinned at **1.2.5**, matching the only proven headless reference), and
  `superdough`, plus a headless render backend (see below).
- **Render backend is decided by a gating validation spike (plan Task 1), not assumed:**
  - *Primary target — pure Node:*
    [`node-web-audio-api`](https://github.com/ircam-ismm/node-web-audio-api) (Rust-backed Web Audio
    for Node, provides `OfflineAudioContext`). Lighter, no browser — **if** it renders superdough
    (including its `AudioWorklet`-based synths/effects) with acceptable fidelity. This is unverified
    and the spike must prove it.
  - *Fallback — headless Chromium:* [Playwright](https://playwright.dev) driving Strudel in a
    headless browser, rendering via an **in-page `OfflineAudioContext`** (still deterministic,
    faster-than-realtime, full worklet fidelity). This is the proven path
    ([live-coding-music-mcp](https://github.com/williamzujkowski/strudel-mcp-server) uses Playwright);
    the cost is a heavier CI (Chromium download).
  - Either backend keeps the firewall and determinism intact; only the "no browser" property and CI
    weight differ. The spike picks one before any rendering code is written against it.
- These are **`devDependencies` of a tool that is never packaged into any shippable artifact.**
  Using AGPL software to *produce* audio does not make the audio a copyleft derivative (same as
  rendering a track in an AGPL DAW). The firewall is: AGPL code lives only in `tools/strudel_render/`,
  which CI runs but never distributes.
- The client/server build graphs have **no dependency** on this tool at runtime — they consume
  the rendered WAV/Ogg like any other audio asset.

## 4. Pipeline

```
 content/<ns>/assets/audio/mus/<zone>/<stem>.strudel   (source of truth, checked in)
 content/<ns>/assets/**/<stem>.asset.yaml              (meridian/asset@1 sidecar, checked in)
        │
        ▼   tools/strudel_render  (CI stage, before content validation/packaging)
   ┌─────────────────────────────────────────────────────────────────┐
   │ 1. Read sidecar → authoritative bpm / time_signature /           │
   │    length_bars / key. Read adjacent render manifest → variant /   │
   │    tail length / sample-bank refs.                                │
   │ 2. Transpile + evaluate .strudel via @strudel/transpiler+core.   │
   │ 3. Enforce sample allowlist (§7); reject on violation.           │
   │ 4. Render N bars (+ wrap tail, §6) through superdough on the      │
   │    spike-selected backend (§3) → 44.1k/16-bit stereo WAV.          │
   │ 5. Write WAV to the sidecar's `source` path (generated artifact). │
   │ 6. Emit/patch provenance block (§7) for lint L022.               │
   └─────────────────────────────────────────────────────────────────┘
        │
        ▼   [UNCHANGED existing pipeline]
   validate_content.py (loudness/lint) → TLS-01 encode (WAV→Ogg) → .mcpack → ZoneMusicPlayer
```

**Artifact policy (decision: sources-only, render-in-CI):** `.strudel` sources + sidecars are the
only checked-in inputs. The WAV at the sidecar's `source` path is a **generated build artifact**
(not committed). Consequence: the **render stage must run before** `validate_content.py` and TLS-01,
because their source-existence, loudness, and encode steps require the WAV to be present. A
`strudel_render --check` mode re-renders and asserts byte-stability for CI verification of a source.

## 5. Determinism & the variation model

Strudel's random modifiers are a **pure function of cycle time `t`** — the same pattern queried
over the same cycle span yields identical events, hence byte-identical audio (given pinned deps).
This is what makes CI-render-from-source sound.

- Render parameters live in a small **adjacent render manifest** (`<stem>.render.yaml`), *not* in
  the `meridian/asset@1` sidecar — the asset schema is `additionalProperties: false` and stays
  frozen (§8). The manifest holds `variant` (cycle offset, applied as `.early(offset)` / seed),
  `tail` length (§6), and the sample-bank references checked against the allowlist (§7). Variants
  `0..N` of one `.strudel` source are distinct assets (distinct `mus.*` IDs + sidecars) sharing the
  source — the offline variant pool that stands in for runtime generativity.
- The **pinned Strudel + superdough + node-web-audio-api versions** are recorded (lockfile +
  a `strudel_version` note in provenance) so renders are stable across machines and over time.
- Known dist quirk encapsulated in the tool: Strudel's npm bundles duplicate the `Pattern` class
  across modules; the renderer calls `setStringParser(mini.mini)` after import so mini-notation
  registers on the same `Pattern` copy that `note()`/`s()` use.

## 6. Seamless looping

`music_stem` sets are tempo/key-locked seamless loops of identical `length_bars` (Music PRD §2.1).

- Loop stems render an integer number of bars at fixed BPM → naturally loopable.
- FX tails (reverb/delay) crossing the loop point are handled by **render-and-wrap**: render
  `length_bars + tail` worth, then fold the overflow tail back over the head so `loop: true`
  stems have no seam.
- A loop-boundary continuity check (start/end sample-neighbourhood energy + click detection)
  validates seamlessness and fails the render on a detectable seam.

## 7. Provenance & license (mapped to `meridian/asset@1`)

License allowlist is **CC0-1.0 / CC-BY-4.0 only** (TD-09; enforced by CI). The renderer derives
and writes the provenance block so lint **L022** passes without hand-editing:

| Pattern uses | `provenance.source_tier` | Required extra fields (per schema `allOf`) |
|--------------|--------------------------|--------------------------------------------|
| Pure synthesis only | `original` | `authors` |
| Any curated CC0 sample bank | `cc0` | `origin_url`, `license_verified_on` (+ per-bank record) |

- **Classification rule to codify:** a hand-written Strudel pattern is **`original`** (human-authored
  algorithmic composition), **never `ai`**. The `ai` tier is reserved for generative-AI tools and
  triggers §3.2 prompt-hygiene requirements that do not apply here.
- **Sample firewall (decision: curated CC0 bank allowed):** the renderer maintains an allowlist of
  CC0 / own sample banks. Any `s(...)` referencing a bank outside the allowlist **fails the build** —
  this is what prevents an un-cleared sample license from being silently baked into a shipped WAV.
  Pure-synthesis patterns always pass. Used banks are recorded in provenance.
- The `.strudel` source is carried in the sidecar's existing **`extra_sources`** field (documented
  there as "stem sessions") so the composition source travels with the asset for reproducibility
  and audit — the audio's "source code."

## 8. Integration with the existing contract

No schema change is required for the core path. A stem sidecar already looks like
([content/core/assets/mus/zone01_tension.asset.yaml](../../../content/core/assets/mus/zone01_tension.asset.yaml)):

```yaml
schema: meridian/asset@1
id: core:mus.zone01.tension
class: music_stem
source: assets/audio/mus/zone01/tension.wav      # generated by strudel_render (not committed)
extra_sources: [assets/audio/mus/zone01/tension.strudel]   # the pattern source (committed)
license: CC-BY-4.0
provenance: { source_tier: original, authors: [meridian-contributors] }   # emitted by renderer
loudness: { lufs_integrated: -16.0, true_peak_dbtp: -1.0 }
music: { stem_set: mus.zone01, layer: L3, bpm: 96, time_signature: "4/4", length_bars: 96, key: d_dorian, loop: true }
```

The renderer reads `music.{bpm,time_signature,length_bars,key}` as the **authoritative** render
parameters, so a stem physically cannot drift from its declared metadata — one source of truth.
`validate_content.py` gains a check that every `music_stem` carrying a `.strudel` extra_source
round-trips (renders, matches declared length, passes the loop-boundary check).

## 9. Testing / verification

- **Renderer unit tests:** determinism (same input → byte-identical WAV under pinned deps);
  rendered length == `length_bars`; loop-boundary continuity; sample-allowlist rejection;
  provenance-tier derivation (synthesis→original, CC0 bank→cc0).
- **End-to-end:** author `.strudel` sources for the three existing placeholder stems
  (`content/core/assets/mus/zone01_{explore,tension,combat}`), render them in CI, and confirm they
  load in `ZoneMusicPlayer`, stay stem-locked across state changes, and pass loudness + lint gates.
- **Provenance gate:** a pattern referencing a non-allowlisted sample bank fails CI (negative test).

## 10. Open risks

1. **Render-backend fidelity (gated, Task 1):** pure-Node `node-web-audio-api` may not support
   superdough's `AudioWorklet` synths/effects, or may differ audibly from browser superdough. The
   plan's first task is a spike that proves pure-Node fidelity or selects the Playwright/headless-
   Chromium fallback. Once a backend is chosen, its render *is* the master (no browser reference to
   match); the risk is a heavier CI if the fallback wins, not a broken pipeline.
2. **Upstream dist quirks:** the `setStringParser`/duplicate-`Pattern` issue and one-render-per-process
   memory behaviour are pinned-version-specific; encapsulated + covered by tests, revisited on bumps.
3. **CI cost:** rendering blocks the Node event loop and is single-render-per-process; batch render is
   parallelized across processes with a concurrency cap. Sources-only means every packaging build must
   render — mitigated by a content-addressed render cache keyed on (source hash, dep versions).
4. **Scope creep:** stingers / ambient beds are deferred; resist folding them in before the stem path
   is proven end-to-end.

## 11. Milestone fit

Slots into the M0 "pipeline proven with 1 adaptive zone track" deliverable (Music PRD §1.3): the
first proof can be a Strudel-authored `mus.zone01` set rendered in CI, satisfying both the audio
pipeline proof and this tool's end-to-end test simultaneously.
