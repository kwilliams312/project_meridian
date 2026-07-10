# Backend Validation Spike — Decision

**Date:** 2026-07-09
**Task:** Plan Task 1 (backend selection spike)
**Decision:** **Playwright + headless Chromium** (in-page `OfflineAudioContext`).
**Rejected:** pure-Node (`node-web-audio-api` + superdough).

---

## TL;DR

Pure Node cannot render superdough's `AudioWorklet`-based synths/effects at all —
`node-web-audio-api`'s `AudioWorklet.addModule()` only resolves **filesystem/module
paths**, and superdough ships its worklets as inline `data:text/javascript;base64,…`
URLs. Every worklet synth/effect (`supersaw`, `coarse`, `crush`, ladder filter)
throws `processor '…' is not registered`. Only plain oscillators + BiquadFilter
render in Node.

Playwright drives real Chromium, whose `OfflineAudioContext` fully supports
data-URL worklets, so **both** pure and worklet patterns render non-silent. This
matches spec §3 (fallback = "the proven path") and §10 risk #1.

---

## Environment

- Node `v26.3.0`, npm `11.16.0`, macOS arm64.
- `superdough@1.2.5` resolves directly on npm (no fallback pin needed).
  `npm view @strudel/webaudio@1.2.5 dependencies` confirms it pairs with
  `superdough@1.2.5`, `@strudel/core@1.2.4` — we pin `@strudel/*@1.2.5`.
- Playwright `1.61.1`; ran `npx playwright install chromium` to fetch the matching
  Chromium build `1228` (an older `1208` was present from a different Playwright).

## Evidence

Peak = max |sample| over the left channel of a 2-cycle (4 s) render; non-silent
threshold = `1e-4`.

### Pure Node (`node-web-audio-api` + superdough), one render per process

| Pattern | Result |
|---|---|
| `note("c4 e4 g4").s("sine")` | ✅ non-silent, peak 0.305 |
| `note("c3 e3 g3").s("sawtooth").lpf(800)` (BiquadFilter) | ✅ non-silent, peak 0.347 |
| `note("c3 e3").s("supersaw")` | ❌ `Failed to construct 'AudioWorkletNode': processor 'supersaw-oscillator' is not registered` |
| `note("c3").s("sawtooth").coarse(4)` | ❌ `processor 'coarse-processor' is not registered` |
| `note("c3").s("sawtooth").crush(4)` | ❌ `processor 'crush-processor' is not registered` |
| `note("c3 e3").s("sawtooth").lpf(600).ftype("ladder")` | ❌ `processor 'ladder-processor' is not registered` |

Root cause (`node_modules/node-web-audio-api/js/AudioWorklet.js:89`):
`addModule()` rejects a `data:` URL with `Cannot resolve module …`; it only
accepts a file path (relative to caller / resolvable via `require.resolve`) or an
`http(s)` URL. superdough loads worklets via `audioWorklet.addModule(dataURL)`
(`dist/index.mjs`), so worklet registration never happens. **Fails the "both
patterns non-silent" gate.**

### Playwright + headless Chromium, **fresh browser context per render**

| Pattern | Result |
|---|---|
| `note("c4 e4 g4").s("sine")` | ✅ peak 0.305 |
| `note("c3 e3 g3").s("sawtooth").lpf(800)` | ✅ peak 0.295 |
| `note("c3 e3").s("supersaw")` (worklet) | ✅ peak 0.260 |
| `note("c3").s("sawtooth").coarse(4)` (worklet) | ✅ peak 0.240 |
| `note("c3").s("sawtooth").crush(4)` (worklet) | ✅ peak 0.250 |
| `note("c3 e3").s("sawtooth").lpf(600).ftype("ladder")` (worklet) | ✅ peak 0.212 |

Backend module end-to-end check (`src/backend/playwright-backend.ts`), 4 bars @
120 bpm 4/4 + 2 s tail → **441000 frames = 10.0 s exactly**, peak 0.299,
non-silent. Length contract (`lengthBars/cps + tailSeconds`) verified.

---

## Verified API (deviations from the plan's probe pseudocode)

Downstream tasks must use these — the plan's Task-1/3 probe sketch was approximate.

1. **Context injection.** superdough has **no** context-injector. `getAudioContext()`
   lazily calls a bare `new AudioContext()` and caches it module-level;
   `setDefaultAudioContext()` takes **no argument** and also does `new AudioContext()`.
   To render offline, override the global constructor so it returns our context:
   `globalThis.AudioContext = function () { return offlineCtx; }` (Node) /
   `window.AudioContext = function () { return offlineCtx; }` (browser). The plan's
   `setDefaultAudioContext(octx)` does **not** work.
2. **`initAudio()` calls `await audioCtx.resume()`** (superdough.mjs:258), which is
   invalid on an `OfflineAudioContext` before `startRendering()`
   (`InvalidStateError: cannot resume an offline context that has not started`).
   No-op it first: `offlineCtx.resume = () => Promise.resolve();`.
3. **Sound registration is required.** After `evalScope`, call
   `registerSynthSounds()` (from `superdough`) or `s("sine")` fails with
   `sound sine not found! Is it loaded?`. `initAudio()` loads the worklet effects
   but does not register the synth waveforms.
4. **Eval flow.** `import { evaluate } from '@strudel/transpiler'`;
   `await evalScope(core, mini, tonal, core.controls)` then
   `core.setStringParser(mini.mini)` (note: `mini.mini`, not `mini`), then
   `const { pattern } = await evaluate(code)`. `evaluate` returns
   `{ mode, pattern, meta }`.
5. **Hap fields.** `pattern.queryArc(0, cycles)` → haps with `hap.whole.begin` /
   `hap.whole.end` (Fraction — use `Number(x.valueOf())`), `hap.value` (the controls
   object), `hap.hasOnset()`. Schedule with
   `superdough(hap.value, tSeconds, durSeconds, cps)` where `t` is the **absolute**
   onset in ctx-seconds (`cycleTime / cps`). superdough refuses `t < ctx.currentTime`,
   so add a tiny lead (`+0.05`) to keep the first onset schedulable.
6. **One render per superdough realm.** superdough caches global effect nodes
   (compressor, reverb, destination gain) on the first context it sees. Reusing a
   page/process for a second render triggers
   `Attempting to connect nodes from different contexts` → silence. The backend uses
   a **fresh Playwright browser context per render**; batch tooling should likewise
   isolate each render (or run one-render-per-process).
7. **In-page globals.** There is no magic `window.superdough` on a blank page. The
   backend esbuild-bundles the pinned `@strudel/*` + `superdough` into an in-page
   IIFE (`src/backend/browser-entry.mjs`) exposing `window.__meridianRender(params)`.
   This keeps the AGPL firewall (deps stay in the dev tool) and avoids a live
   `strudel.cc` network dependency.
8. **Secure context.** `OfflineAudioContext.audioWorklet` is `undefined` on
   `about:blank`/`data:` pages (`isSecureContext === false`). Serve the page from a
   `http://localhost` origin (Chromium treats it as secure).

## cps / length convention

- `cps = (bpm / 60) / beatsPerBar(time_signature)` (e.g. 120 bpm, 4/4 → 0.5).
- 1 cycle == 1 bar. Query cycles = `totalSeconds * cps`.
- `totalSeconds = lengthBars / cps + tailSeconds` (render loop **plus** FX tail so
  the Task-7 loop-wrap has a tail to fold).

## Known follow-ups (not blockers for Task 1/2)

- **Determinism (Task 10):** superdough's `supersaw` worklet seeds phase with
  `Math.random()` → non-deterministic. The zone01 first drop (Task 13) is pure
  synthesis; advanced worklet oscillators will need a seeded/`disableWorklets`
  path or authoring guidance before they can back committed stems.
- **CI weight:** the Playwright path needs a Chromium download
  (`npx playwright install --with-deps chromium`), as anticipated in spec §3/§10.
