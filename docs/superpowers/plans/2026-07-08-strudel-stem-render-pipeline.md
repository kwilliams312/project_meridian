# Strudel Stem-Render Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A build-time tool that renders developer-authored Strudel patterns into deterministic WAV stems conforming to the existing `music_stem` asset contract, with no Strudel/JS in the shipped client or server.

**Architecture:** New isolated Node/TypeScript tool at `tools/strudel_render/`. It reads a `meridian/asset@1` sidecar (authoritative bpm / time_signature / length_bars / key) plus an adjacent `<stem>.render.yaml` (variant / tail / sample-bank refs), evaluates the `.strudel` source via pinned `@strudel/*` (1.2.5), renders audio through a **spike-selected backend** (pure-Node `node-web-audio-api` if it proves faithful, else Playwright + headless Chromium via in-page `OfflineAudioContext`), wraps FX tails for seamless loops, verifies loop seams, derives provenance, and writes a 16-bit/44.1 kHz stereo WAV to the sidecar's `source` path. WAVs are generated build artifacts (not committed); `.strudel` + manifests are the checked-in source of truth. The tool is a dev dependency only — the AGPL firewall (§3 of the spec).

**Tech Stack:** Node ≥22 (repo CI runs Node 26), TypeScript (ESM), `tsx` (run), `vitest` (test), `js-yaml` (YAML), pinned `@strudel/core|mini|tonal|transpiler@1.2.5` + `superdough`, and one of `node-web-audio-api` / `playwright` (decided in Task 1). Python side: one lint rule added to `tools/validate_content.py`. CI: a new job in `.github/workflows/content-ci.yml` + a render step in `scripts/content-build.sh`.

**Spec:** [docs/superpowers/specs/2026-07-08-strudel-offline-stem-pipeline-design.md](../specs/2026-07-08-strudel-offline-stem-pipeline-design.md)

---

## File Structure

```
tools/strudel_render/
  package.json              Pinned deps; scripts: build, test, render, render:check
  tsconfig.json             ESM, strict, NodeNext
  vitest.config.ts          Test config
  .gitignore                node_modules/, .render-cache/, dist/
  README.md                 What it is, how to run, AGPL-firewall note
  sample-banks.allowlist.yaml   CC0/own sample-bank registry (id → origin/license/date)
  src/
    types.ts               RenderParams, StereoBuffer, RenderBackend, StemMeta, RenderManifest
    sidecar.ts             Load + validate asset sidecar & <stem>.render.yaml → StemMeta+RenderManifest
    backend/
      index.ts             selectBackend(): returns the RenderBackend chosen by Task 1
      node-backend.ts      Pure-Node backend (superdough on node-web-audio-api) — if spike passes
      playwright-backend.ts  Headless-Chromium backend (in-page OfflineAudioContext) — fallback
    samples.ts             Extract sample names used by a pattern; enforce allowlist
    provenance.ts          Derive provenance block (original | cc0) from sample usage
    loop.ts                render-and-wrap tail folding + seam detection
    wav.ts                 StereoBuffer → 16-bit PCM WAV bytes
    cache.ts               Content-addressed render cache (source+deps+params → wav)
    render.ts              Orchestrator: sidecar → backend → wrap → seam → wav → provenance
    cli.ts                 CLI: render <glob> [--check] [--all]
  test/
    wav.test.ts  samples.test.ts  provenance.test.ts  loop.test.ts
    sidecar.test.ts  cache.test.ts  determinism.test.ts  render.e2e.test.ts
  fixtures/
    synth.strudel  synth.render.yaml  synth.asset.yaml
    with-sample.strudel  with-sample.render.yaml
    bad-sample.strudel  bad-sample.render.yaml

content/core/assets/audio/mus/zone01/
    explore.strudel  explore.render.yaml   (+ tension, combat)  — E2E, Task 13

tools/validate_content.py   MODIFY: add L023 (strudel-backed stem needs render manifest)
scripts/content-build.sh    MODIFY: render .strudel stems before mcc emit
.github/workflows/content-ci.yml  MODIFY: add strudel-render job
```

**Backend abstraction:** every task after Task 1 programs against the `RenderBackend` interface (Task 2), so the undecided backend internals are isolated to one file. Task 1 (spike) fills in `node-backend.ts` or `playwright-backend.ts` and wires `backend/index.ts`.

---

## Task 1: Backend validation spike (decides render mechanism)

**Goal:** Prove whether pure-Node renders superdough faithfully, or select the Playwright fallback. Produces a working `RenderBackend` in one of the two backend files and a short decision note. This is a spike — its deliverable is a verified backend module, not tests yet.

**Files:**
- Create: `tools/strudel_render/package.json`, `tsconfig.json`, `.gitignore`
- Create: `tools/strudel_render/src/types.ts` (RenderBackend + StereoBuffer only, for the spike)
- Create: `tools/strudel_render/spike/probe-node.mjs`, `tools/strudel_render/spike/probe-playwright.mjs`
- Create: `tools/strudel_render/spike/DECISION.md`

- [ ] **Step 1: Scaffold the Node project**

`tools/strudel_render/package.json`:
```json
{
  "name": "@meridian/strudel-render",
  "private": true,
  "type": "module",
  "engines": { "node": ">=22" },
  "scripts": {
    "spike:node": "node spike/probe-node.mjs",
    "spike:pw": "node spike/probe-playwright.mjs"
  },
  "dependencies": {
    "@strudel/core": "1.2.5",
    "@strudel/mini": "1.2.5",
    "@strudel/tonal": "1.2.5",
    "@strudel/transpiler": "1.2.5",
    "superdough": "1.2.5"
  },
  "devDependencies": {
    "node-web-audio-api": "^1.0.0",
    "playwright": "^1.52.0"
  }
}
```

`tools/strudel_render/.gitignore`:
```
node_modules/
dist/
.render-cache/
```

- [ ] **Step 2: Install deps**

Run: `cd tools/strudel_render && npm install`
Expected: lockfile written, `@strudel/*@1.2.5` resolved. If `superdough@1.2.5` does not exist as a standalone version, pin to the version `@strudel/webaudio@1.2.5` depends on (read `npm view @strudel/webaudio@1.2.5 dependencies`) and record it in DECISION.md.

- [ ] **Step 3: Probe the pure-Node path**

`spike/probe-node.mjs` — render a **worklet-using** pattern (a filtered synth) 2 cycles to WAV and a **pure `note()`** pattern, to test worklet support specifically:
```js
import { AudioContext, OfflineAudioContext } from 'node-web-audio-api';
import { evalScope, controls } from '@strudel/core';
import { mini } from '@strudel/mini';
import { transpiler } from '@strudel/transpiler';
import * as core from '@strudel/core';
// superdough drives Web Audio; give it our OfflineAudioContext.
import { superdough, initAudioOnFirstClick, getAudioContext, setDefaultAudioContext } from 'superdough';

// Known dist quirk: mini registers its string parser on a *different* Pattern
// copy than the one note()/s() use unless we re-point it explicitly.
core.setStringParser(mini);

const SR = 44100, cycles = 2, cps = 0.5; // 1 cycle = 2s
const octx = new OfflineAudioContext({ numberOfChannels: 2, length: SR * (cycles / cps), sampleRate: SR });
setDefaultAudioContext?.(octx); // if superdough exposes an injector; else see DECISION notes

// Two patterns: worklet-heavy (lpf/coarse) and pure sine.
const patterns = {
  worklet: `note("c3 e3 g3 c4").s("sawtooth").lpf(800).room(0.3)`,
  pure:    `note("c4 e4 g4").s("sine")`,
};
for (const [name, code] of Object.entries(patterns)) {
  const { pattern } = transpiler(code);
  const haps = pattern.queryArc(0, cycles); // events with .whole and controls
  // schedule each hap by calling superdough(hap.value, time, duration) on octx timeline
  for (const h of haps) {
    const t = h.whole.begin.valueOf() / cps;
    const dur = (h.whole.end.valueOf() - h.whole.begin.valueOf()) / cps;
    superdough(h.value, t, dur);
  }
  const buf = await octx.startRendering();
  const nonSilent = buf.getChannelData(0).some(x => Math.abs(x) > 1e-4);
  console.log(name, 'nonSilent=', nonSilent);
}
```
Run: `npm run spike:node`
Record in DECISION.md: does it import, run, and produce non-silent audio for **both** patterns? Note any API mismatch (the exact superdough context-injection call, hap field names) so Task 2 uses the real API.

- [ ] **Step 4: Probe the Playwright fallback**

`spike/probe-playwright.mjs` — load `https://strudel.cc` (or a local bundle) in headless Chromium, evaluate a pattern into an in-page `OfflineAudioContext`, and pull the rendered PCM back:
```js
import { chromium } from 'playwright';
const browser = await chromium.launch();
const page = await browser.newPage();
await page.goto('https://strudel.cc/', { waitUntil: 'networkidle' });
const pcm = await page.evaluate(async () => {
  // strudel globals are present on the REPL page; render 2 cycles offline.
  const SR = 44100, cycles = 2, cps = 0.5;
  const octx = new OfflineAudioContext(2, SR * (cycles / cps), SR);
  // window.superdough / window.evaluate exist on the REPL — exact names verified here
  // (record them in DECISION.md). Schedule haps, then:
  const buf = await octx.startRendering();
  return [Array.from(buf.getChannelData(0)), Array.from(buf.getChannelData(1))];
});
console.log('pw nonSilent=', pcm[0].some(x => Math.abs(x) > 1e-4));
await browser.close();
```
Run: `npx playwright install chromium && npm run spike:pw`
Record whether it renders non-silent audio and the exact in-page global names.

- [ ] **Step 5: Decide and write the backend**

In `DECISION.md`, choose: **pure-Node if Step 3 produced non-silent audio for BOTH patterns**; otherwise **Playwright**. Then implement the chosen backend as a module exporting the Task-2 interface. Define `src/types.ts` now:
```ts
export interface StereoBuffer { left: Float32Array; right: Float32Array; sampleRate: number; }
export interface RenderParams {
  code: string; bpm: number; timeSignature: string; lengthBars: number;
  key?: string; variant: number; tailSeconds: number; sampleRate: number;
}
export interface RenderBackend { render(p: RenderParams): Promise<StereoBuffer>; }
```
Create `src/backend/node-backend.ts` **or** `src/backend/playwright-backend.ts` implementing `RenderBackend` with the verified API from the probe. Compute cycles from bars: `cycles = lengthBars / barsPerCycle` (1 cycle = 1 bar unless the pattern sets otherwise; document the convention). Convert `bpm`+`time_signature` to `cps`: `cps = (bpm / 60) / beatsPerBar`.

- [ ] **Step 6: Commit the spike + backend**

```bash
git add tools/strudel_render/package.json tools/strudel_render/package-lock.json \
  tools/strudel_render/tsconfig.json tools/strudel_render/.gitignore \
  tools/strudel_render/src/types.ts tools/strudel_render/src/backend \
  tools/strudel_render/spike
git commit -m "feat(strudel-render): backend spike + selected render backend"
```

`tsconfig.json`:
```json
{
  "compilerOptions": {
    "target": "ES2022", "module": "NodeNext", "moduleResolution": "NodeNext",
    "strict": true, "esModuleInterop": true, "skipLibCheck": true,
    "outDir": "dist", "rootDir": "src", "declaration": false
  },
  "include": ["src"]
}
```

---

## Task 2: Test harness + backend interface wiring

**Files:**
- Modify: `tools/strudel_render/package.json` (add vitest, tsx, scripts)
- Create: `tools/strudel_render/vitest.config.ts`
- Create: `tools/strudel_render/src/backend/index.ts`

- [ ] **Step 1: Add test tooling**

Add to `devDependencies`: `"vitest": "^2.0.0"`, `"tsx": "^4.0.0"`, `"typescript": "^5.5.0"`, `"@types/node": "^22"`, `"js-yaml": "^4.1.0"`, `"@types/js-yaml": "^4.0.9"`. Add scripts:
```json
"scripts": {
  "build": "tsc -p tsconfig.json",
  "test": "vitest run",
  "render": "tsx src/cli.ts",
  "render:check": "tsx src/cli.ts --all --check"
}
```
Run: `npm install`
Expected: vitest + tsx installed.

- [ ] **Step 2: vitest config**

`vitest.config.ts`:
```ts
import { defineConfig } from 'vitest/config';
export default defineConfig({ test: { environment: 'node', include: ['test/**/*.test.ts'] } });
```

- [ ] **Step 3: Backend selector**

`src/backend/index.ts` (import whichever file Task 1 created):
```ts
import type { RenderBackend } from '../types.js';
// One of these exists after Task 1; import the selected one.
import { backend } from './node-backend.js'; // or './playwright-backend.js'
export function selectBackend(): RenderBackend { return backend; }
```

- [ ] **Step 4: Smoke test the selector**

`test/sidecar.test.ts` placeholder that asserts `selectBackend()` returns an object with a `render` function:
```ts
import { expect, test } from 'vitest';
import { selectBackend } from '../src/backend/index.js';
test('selectBackend returns a RenderBackend', () => {
  expect(typeof selectBackend().render).toBe('function');
});
```
Run: `npm test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/package.json tools/strudel_render/package-lock.json \
  tools/strudel_render/vitest.config.ts tools/strudel_render/src/backend/index.ts \
  tools/strudel_render/test/sidecar.test.ts
git commit -m "feat(strudel-render): test harness + backend selector"
```

---

## Task 3: WAV writer (16-bit/44.1 kHz stereo)

**Files:**
- Create: `tools/strudel_render/src/wav.ts`
- Test: `tools/strudel_render/test/wav.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { encodeWav } from '../src/wav.js';
test('encodeWav writes a 44-byte RIFF header with correct sizes', () => {
  const sr = 44100, n = 100;
  const buf = { left: new Float32Array(n), right: new Float32Array(n), sampleRate: sr };
  const bytes = encodeWav(buf);
  expect(bytes.subarray(0, 4).toString('ascii')).toBe('RIFF');
  expect(bytes.subarray(8, 12).toString('ascii')).toBe('WAVE');
  // data chunk = n frames * 2 channels * 2 bytes
  expect(bytes.readUInt32LE(40)).toBe(n * 2 * 2);
  expect(bytes.length).toBe(44 + n * 2 * 2);
});
test('encodeWav clamps and quantizes full-scale samples', () => {
  const buf = { left: new Float32Array([1, -1, 2]), right: new Float32Array([0, 0, 0]), sampleRate: 44100 };
  const b = encodeWav(buf);
  expect(b.readInt16LE(44)).toBe(32767);       // +1.0 → +full scale
  expect(b.readInt16LE(48)).toBe(-32768);      // -1.0 → -full scale
  expect(b.readInt16LE(52)).toBe(32767);       // +2.0 clamped
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm test -- wav`
Expected: FAIL ("encodeWav is not a function").

- [ ] **Step 3: Implement**

`src/wav.ts`:
```ts
import type { StereoBuffer } from './types.js';
export function encodeWav({ left, right, sampleRate }: StereoBuffer): Buffer {
  const n = Math.min(left.length, right.length);
  const dataBytes = n * 2 * 2;
  const b = Buffer.alloc(44 + dataBytes);
  b.write('RIFF', 0, 'ascii'); b.writeUInt32LE(36 + dataBytes, 4); b.write('WAVE', 8, 'ascii');
  b.write('fmt ', 12, 'ascii'); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22);
  b.writeUInt32LE(sampleRate, 24); b.writeUInt32LE(sampleRate * 2 * 2, 28);
  b.writeUInt16LE(2 * 2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36, 'ascii'); b.writeUInt32LE(dataBytes, 40);
  const q = (x: number) => { const v = Math.max(-1, Math.min(1, x)); return v < 0 ? v * 32768 : v * 32767; };
  let off = 44;
  for (let i = 0; i < n; i++) { b.writeInt16LE(Math.round(q(left[i])), off); b.writeInt16LE(Math.round(q(right[i])), off + 2); off += 4; }
  return b;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm test -- wav`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/src/wav.ts tools/strudel_render/test/wav.test.ts
git commit -m "feat(strudel-render): 16-bit stereo WAV encoder"
```

---

## Task 4: Sidecar + render-manifest loader

**Files:**
- Create: `tools/strudel_render/src/sidecar.ts`
- Test: `tools/strudel_render/test/sidecar.test.ts` (extend)
- Create fixtures: `tools/strudel_render/fixtures/synth.asset.yaml`, `synth.render.yaml`, `synth.strudel`

- [ ] **Step 1: Write fixtures**

`fixtures/synth.asset.yaml`:
```yaml
schema: meridian/asset@1
id: core:mus.fixture.synth
class: music_stem
source: assets/audio/mus/fixture/synth.wav
extra_sources: [assets/audio/mus/fixture/synth.strudel]
license: CC0-1.0
provenance: { source_tier: original, authors: [test] }
loudness: { lufs_integrated: -16.0, true_peak_dbtp: -1.0 }
music: { stem_set: mus.fixture, layer: L1, bpm: 120, time_signature: "4/4", length_bars: 4, key: c_major, loop: true }
```
`fixtures/synth.render.yaml`:
```yaml
schema: meridian/strudel-render@1
pattern: assets/audio/mus/fixture/synth.strudel
variant: 0
tail_seconds: 2.0
sample_banks: []
```
`fixtures/synth.strudel`:
```
note("c4 e4 g4 c5").s("sine").slow(4)
```

- [ ] **Step 2: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { loadStem } from '../src/sidecar.js';
test('loadStem merges music metadata with render manifest into RenderParams', async () => {
  const stem = await loadStem('fixtures/synth.asset.yaml');
  expect(stem.params.bpm).toBe(120);
  expect(stem.params.lengthBars).toBe(4);
  expect(stem.params.timeSignature).toBe('4/4');
  expect(stem.params.variant).toBe(0);
  expect(stem.params.tailSeconds).toBe(2.0);
  expect(stem.manifest.sample_banks).toEqual([]);
  expect(stem.wavOutPath).toMatch(/synth\.wav$/);
});
test('loadStem throws when the render manifest is missing', async () => {
  await expect(loadStem('fixtures/no-manifest.asset.yaml')).rejects.toThrow(/render manifest/i);
});
```

- [ ] **Step 3: Run test to verify it fails**

Run: `npm test -- sidecar`
Expected: FAIL ("loadStem is not a function").

- [ ] **Step 4: Implement**

`src/sidecar.ts`:
```ts
import { readFile } from 'node:fs/promises';
import { dirname, resolve, basename } from 'node:path';
import yaml from 'js-yaml';
import type { RenderParams } from './types.js';

export interface RenderManifest { schema: string; pattern: string; variant: number; tail_seconds: number; sample_banks: string[]; }
export interface Stem { id: string; params: RenderParams; manifest: RenderManifest; codePath: string; wavOutPath: string; sidecar: any; sidecarPath: string; }

function beatsPerBar(sig: string): number { return Number(sig.split('/')[0]); }

export async function loadStem(sidecarPath: string): Promise<Stem> {
  const sidecar: any = yaml.load(await readFile(sidecarPath, 'utf8'));
  if (sidecar?.class !== 'music_stem') throw new Error(`${sidecarPath}: not a music_stem`);
  const dir = dirname(sidecarPath);
  const manifestPath = resolve(dir, basename(sidecarPath).replace(/\.asset\.ya?ml$/, '.render.yaml'));
  let manifest: RenderManifest;
  try { manifest = yaml.load(await readFile(manifestPath, 'utf8')) as RenderManifest; }
  catch { throw new Error(`${sidecarPath}: missing render manifest at ${manifestPath}`); }
  const codePath = resolve(dir, basename(manifest.pattern));
  const code = await readFile(codePath, 'utf8');
  const m = sidecar.music;
  const params: RenderParams = {
    code, bpm: m.bpm, timeSignature: m.time_signature, lengthBars: m.length_bars,
    key: m.key, variant: manifest.variant ?? 0, tailSeconds: manifest.tail_seconds ?? 0, sampleRate: 44100,
  };
  void beatsPerBar; // used by backend for cps; exported for reuse
  const wavOutPath = resolve(dir, basename(sidecar.source));
  return { id: sidecar.id, params, manifest, codePath, wavOutPath, sidecar, sidecarPath };
}
export { beatsPerBar };
```
Add a `fixtures/no-manifest.asset.yaml` (copy of synth.asset.yaml, id `...no_manifest`, with no sibling `.render.yaml`) so the negative test resolves a real file.

- [ ] **Step 5: Run test to verify it passes**

Run: `npm test -- sidecar`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add tools/strudel_render/src/sidecar.ts tools/strudel_render/test/sidecar.test.ts tools/strudel_render/fixtures
git commit -m "feat(strudel-render): sidecar + render-manifest loader"
```

---

## Task 5: Sample-bank allowlist enforcement

**Files:**
- Create: `tools/strudel_render/src/samples.ts`
- Create: `tools/strudel_render/sample-banks.allowlist.yaml`
- Test: `tools/strudel_render/test/samples.test.ts`

- [ ] **Step 1: Allowlist registry**

`sample-banks.allowlist.yaml`:
```yaml
# CC0 / own sample banks cleared for shipped renders. Each entry feeds provenance.
schema: meridian/strudel-sample-allowlist@1
banks:
  meridian_perc:
    origin_url: https://example.invalid/meridian-perc   # own bank, CC0
    license: CC0-1.0
    license_verified_on: "2026-07-08"
```

- [ ] **Step 2: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { usedSampleBanks, enforceAllowlist } from '../src/samples.js';
test('usedSampleBanks finds s()/sound() bank names', () => {
  expect(usedSampleBanks(`note("c e g").s("sine")`)).toEqual([]); // synth waveforms are not banks
  expect(usedSampleBanks(`s("bd sd bd sd")`).sort()).toEqual(['bd', 'sd']);
  expect(usedSampleBanks(`sound("meridian_perc:2")`)).toEqual(['meridian_perc']);
});
test('enforceAllowlist rejects non-allowlisted banks', async () => {
  await expect(enforceAllowlist(['bd'], 'fixtures')).rejects.toThrow(/not on the CC0 allowlist/i);
});
test('enforceAllowlist accepts allowlisted banks and returns their records', async () => {
  const recs = await enforceAllowlist(['meridian_perc'], '.');
  expect(recs[0].license).toBe('CC0-1.0');
});
```
(Synth waveform names `sine/sawtooth/square/triangle` are treated as synthesis, not sample banks — maintain a `SYNTH_SOURCES` set.)

- [ ] **Step 3: Run test to verify it fails**

Run: `npm test -- samples`
Expected: FAIL.

- [ ] **Step 4: Implement**

`src/samples.ts`:
```ts
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import yaml from 'js-yaml';

const SYNTH_SOURCES = new Set(['sine', 'sawtooth', 'saw', 'square', 'triangle', 'tri', 'pulse', 'white', 'pink', 'brown', 'z_sawtooth', 'supersaw']);
export interface BankRecord { origin_url: string; license: string; license_verified_on: string; }

// Static scan: collect string args to s(...) / sound(...). A bank ref is the part before ':'.
export function usedSampleBanks(code: string): string[] {
  const banks = new Set<string>();
  const re = /\b(?:s|sound)\s*\(\s*(["'`])([^"'`]+)\1/g;
  for (let m; (m = re.exec(code)); ) {
    for (const tok of m[2].split(/\s+/)) {
      const name = tok.split(':')[0].replace(/[<>[\]!@*/?.]/g, '').trim();
      if (name && !SYNTH_SOURCES.has(name) && !/^~?\d+$/.test(name)) banks.add(name);
    }
  }
  return [...banks];
}

export async function enforceAllowlist(banks: string[], repoRootForAllowlist: string): Promise<BankRecord[]> {
  if (banks.length === 0) return [];
  const allowPath = resolve(repoRootForAllowlist, 'sample-banks.allowlist.yaml');
  const doc: any = yaml.load(await readFile(allowPath, 'utf8'));
  const table = doc?.banks ?? {};
  const out: BankRecord[] = [];
  for (const b of banks) {
    if (!table[b]) throw new Error(`sample bank "${b}" is not on the CC0 allowlist (${allowPath})`);
    out.push(table[b]);
  }
  return out;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `npm test -- samples`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add tools/strudel_render/src/samples.ts tools/strudel_render/sample-banks.allowlist.yaml tools/strudel_render/test/samples.test.ts
git commit -m "feat(strudel-render): sample-bank allowlist firewall"
```

---

## Task 6: Provenance derivation

**Files:**
- Create: `tools/strudel_render/src/provenance.ts`
- Test: `tools/strudel_render/test/provenance.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { deriveProvenance } from '../src/provenance.js';
test('pure synthesis → source_tier original', () => {
  const p = deriveProvenance([], ['meridian-contributors']);
  expect(p).toEqual({ source_tier: 'original', authors: ['meridian-contributors'] });
});
test('CC0 sample use → source_tier cc0 with origin + verified date', () => {
  const p = deriveProvenance(
    [{ origin_url: 'https://x.invalid/b', license: 'CC0-1.0', license_verified_on: '2026-07-08' }],
    ['meridian-contributors']);
  expect(p.source_tier).toBe('cc0');
  expect(p.origin_url).toBe('https://x.invalid/b');
  expect(p.license_verified_on).toBe('2026-07-08');
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm test -- provenance`
Expected: FAIL.

- [ ] **Step 3: Implement**

`src/provenance.ts`:
```ts
import type { BankRecord } from './samples.js';
export interface Provenance {
  source_tier: 'original' | 'cc0';
  authors: string[];
  origin_url?: string;
  license_verified_on?: string;
}
export function deriveProvenance(banks: BankRecord[], authors: string[]): Provenance {
  if (banks.length === 0) return { source_tier: 'original', authors };
  // Any CC0 sample use → cc0 tier. Use the first bank's origin/date (schema requires the fields;
  // multi-bank aggregation into attribution is a follow-up if CC-BY is ever allowed).
  const b = banks[0];
  return { source_tier: 'cc0', authors, origin_url: b.origin_url, license_verified_on: b.license_verified_on };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm test -- provenance`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/src/provenance.ts tools/strudel_render/test/provenance.test.ts
git commit -m "feat(strudel-render): provenance derivation (original|cc0)"
```

---

## Task 7: Seamless-loop tail wrap + seam detection

**Files:**
- Create: `tools/strudel_render/src/loop.ts`
- Test: `tools/strudel_render/test/loop.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { wrapTail, seamDiscontinuity } from '../src/loop.js';
test('wrapTail folds the tail region back over the head and truncates to loop length', () => {
  const sr = 10, loopN = 10, tailN = 3;
  const left = new Float32Array(loopN + tailN); const right = new Float32Array(loopN + tailN);
  left[10] = 0.5; left[11] = 0.25; left[12] = 0.1; // tail energy after the loop point
  const out = wrapTail({ left, right, sampleRate: sr }, loopN);
  expect(out.left.length).toBe(loopN);
  expect(out.left[0]).toBeCloseTo(0.5);   // tail[0] folded onto head[0]
  expect(out.left[1]).toBeCloseTo(0.25);
});
test('seamDiscontinuity is ~0 for a continuous loop and large for a hard cut', () => {
  const sr = 100; const n = 100;
  const cont = new Float32Array(n); for (let i=0;i<n;i++) cont[i] = Math.sin(2*Math.PI*i/n);
  expect(seamDiscontinuity({ left: cont, right: cont, sampleRate: sr })).toBeLessThan(0.05);
  const cut = new Float32Array(n); for (let i=0;i<n;i++) cut[i] = i < n/2 ? 0.9 : -0.9;
  expect(seamDiscontinuity({ left: cut, right: cut, sampleRate: sr })).toBeGreaterThan(0.5);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm test -- loop`
Expected: FAIL.

- [ ] **Step 3: Implement**

`src/loop.ts`:
```ts
import type { StereoBuffer } from './types.js';
// Render produced loopN + tail samples; fold the tail back onto the head, keep loopN.
export function wrapTail(buf: StereoBuffer, loopN: number): StereoBuffer {
  const fold = (a: Float32Array) => {
    const out = a.slice(0, loopN);
    for (let i = loopN; i < a.length; i++) out[i - loopN] += a[i];
    return out;
  };
  return { left: fold(buf.left), right: fold(buf.right), sampleRate: buf.sampleRate };
}
// Discontinuity = |value at loop end wrapping to start| gradient vs local RMS. 0 = seamless.
export function seamDiscontinuity(buf: StereoBuffer): number {
  const a = buf.left; const n = a.length;
  const jump = Math.abs(a[0] - a[n - 1]);
  let sum = 0; for (let i = 0; i < n; i++) sum += a[i] * a[i];
  const rms = Math.sqrt(sum / n) || 1e-9;
  return jump / (rms * 4); // normalize; hard cut ≫ continuous
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm test -- loop`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/src/loop.ts tools/strudel_render/test/loop.test.ts
git commit -m "feat(strudel-render): seamless-loop tail wrap + seam detection"
```

---

## Task 8: Content-addressed render cache

**Files:**
- Create: `tools/strudel_render/src/cache.ts`
- Test: `tools/strudel_render/test/cache.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { cacheKey } from '../src/cache.js';
test('cacheKey is stable for identical inputs and changes when any input changes', () => {
  const base = { code: 'note("c")', bpm: 120, timeSignature: '4/4', lengthBars: 4, variant: 0, tailSeconds: 2, sampleRate: 44100 } as any;
  const deps = { '@strudel/core': '1.2.5', superdough: '1.2.5', backend: 'node' };
  const k1 = cacheKey(base, deps);
  const k2 = cacheKey({ ...base }, { ...deps });
  const k3 = cacheKey({ ...base, variant: 1 }, deps);
  const k4 = cacheKey(base, { ...deps, superdough: '1.2.6' });
  expect(k1).toBe(k2);
  expect(k1).not.toBe(k3);
  expect(k1).not.toBe(k4);
  expect(k1).toMatch(/^[0-9a-f]{64}$/);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm test -- cache`
Expected: FAIL.

- [ ] **Step 3: Implement**

`src/cache.ts`:
```ts
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import type { RenderParams } from './types.js';

export function cacheKey(p: RenderParams, deps: Record<string, string>): string {
  const h = createHash('sha256');
  h.update(JSON.stringify({
    code: p.code, bpm: p.bpm, sig: p.timeSignature, bars: p.lengthBars,
    key: p.key ?? null, variant: p.variant, tail: p.tailSeconds, sr: p.sampleRate,
    deps: Object.keys(deps).sort().map(k => `${k}@${deps[k]}`),
  }));
  return h.digest('hex');
}
const DIR = resolve('.render-cache');
export async function cacheGet(key: string): Promise<Buffer | null> {
  try { return await readFile(resolve(DIR, `${key}.wav`)); } catch { return null; }
}
export async function cachePut(key: string, wav: Buffer): Promise<void> {
  await mkdir(DIR, { recursive: true }); await writeFile(resolve(DIR, `${key}.wav`), wav);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm test -- cache`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/src/cache.ts tools/strudel_render/test/cache.test.ts
git commit -m "feat(strudel-render): content-addressed render cache"
```

---

## Task 9: Render orchestrator

**Files:**
- Create: `tools/strudel_render/src/render.ts`
- Test: `tools/strudel_render/test/render.e2e.test.ts` (real backend, one fixture)

**Depends on the backend from Task 1.** Uses `selectBackend()`.

- [ ] **Step 1: Write the failing test (real render of the synth fixture)**

```ts
import { expect, test } from 'vitest';
import { renderStem } from '../src/render.js';
test('renderStem produces a non-silent WAV of the declared length and clean provenance', async () => {
  const r = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.' });
  // 4 bars @ 120 bpm / (4 beats/bar) = 8s → 8*44100 frames
  const frames = (r.wav.length - 44) / 4;
  expect(frames).toBeGreaterThan(44100 * 7);   // ~8s, allow rounding
  expect(frames).toBeLessThan(44100 * 9);
  expect(r.provenance.source_tier).toBe('original');
  expect(r.seam).toBeLessThan(0.2);            // seamless
  const pcm = r.wav.subarray(44);
  expect(pcm.some((_, i) => i % 2 === 0 && Math.abs(pcm.readInt16LE(i)) > 100)).toBe(true); // non-silent
}, 60_000);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm test -- render.e2e`
Expected: FAIL ("renderStem is not a function").

- [ ] **Step 3: Implement**

`src/render.ts`:
```ts
import { loadStem, beatsPerBar } from './sidecar.js';
import { selectBackend } from './backend/index.js';
import { usedSampleBanks, enforceAllowlist } from './samples.js';
import { deriveProvenance, type Provenance } from './provenance.js';
import { wrapTail, seamDiscontinuity } from './loop.js';
import { encodeWav } from './wav.js';
import { cacheKey, cacheGet, cachePut } from './cache.js';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
function depVersions(backend: string): Record<string, string> {
  const v = (n: string) => require(`${n}/package.json`).version;
  return { '@strudel/core': v('@strudel/core'), superdough: v('superdough'), backend };
}

export interface RenderResult { wav: Buffer; provenance: Provenance; seam: number; wavOutPath: string; }

export async function renderStem(sidecarPath: string, opts: { repoRoot: string; backendName?: string }): Promise<RenderResult> {
  const stem = await loadStem(sidecarPath);
  const banks = usedSampleBanks(stem.params.code);
  const records = await enforceAllowlist(banks, opts.repoRoot === '.' ? 'tools/strudel_render' : `${opts.repoRoot}/tools/strudel_render`);
  const provenance = deriveProvenance(records, stem.sidecar.provenance?.authors ?? ['meridian-contributors']);

  const backendName = opts.backendName ?? 'node';
  const key = cacheKey(stem.params, depVersions(backendName));
  let wav = await cacheGet(key);
  let seam = 0;
  if (!wav) {
    const beats = beatsPerBar(stem.params.timeSignature);
    const raw = await selectBackend().render(stem.params); // renders lengthBars + tailSeconds
    const loopN = Math.round((stem.params.lengthBars * beats) / (stem.params.bpm / 60) * stem.params.sampleRate);
    const looped = stem.sidecar.music.loop === false ? raw : wrapTail(raw, loopN);
    seam = seamDiscontinuity(looped);
    wav = encodeWav(looped);
    await cachePut(key, wav);
  }
  return { wav, provenance, seam, wavOutPath: stem.wavOutPath };
}
```
(The backend renders `lengthBars` **plus** `tailSeconds` worth of audio so `wrapTail` has a tail to fold; document this contract in `backend/index.ts`.)

- [ ] **Step 4: Run test to verify it passes**

Run: `npm test -- render.e2e`
Expected: PASS (may take seconds; timeout set to 60 s).

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/src/render.ts tools/strudel_render/test/render.e2e.test.ts
git commit -m "feat(strudel-render): render orchestrator (allowlist→render→wrap→seam→wav)"
```

---

## Task 10: Determinism test (byte-identical re-render)

**Files:**
- Test: `tools/strudel_render/test/determinism.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { expect, test } from 'vitest';
import { renderStem } from '../src/render.js';
import { rm } from 'node:fs/promises';
test('same source renders byte-identical WAV across runs (cache cleared)', async () => {
  await rm('.render-cache', { recursive: true, force: true });
  const a = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.' });
  await rm('.render-cache', { recursive: true, force: true });
  const b = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.' });
  expect(Buffer.compare(a.wav, b.wav)).toBe(0);
}, 120_000);
```

- [ ] **Step 2: Run to verify**

Run: `npm test -- determinism`
Expected: PASS. **If it FAILS**, the backend introduces nondeterminism (uninitialized buffers, timing jitter, or a non-seeded random). Fix in the backend before proceeding — determinism is a hard requirement (spec §5, and the mcc golden gate). Common fixes: pin `OfflineAudioContext` length exactly; ensure the pattern uses no wall-clock; zero-fill output buffers.

- [ ] **Step 3: Commit**

```bash
git add tools/strudel_render/test/determinism.test.ts
git commit -m "test(strudel-render): byte-identical render determinism gate"
```

---

## Task 11: CLI (render / --check / --all)

**Files:**
- Create: `tools/strudel_render/src/cli.ts`

- [ ] **Step 1: Write the failing test**

`test/cli.test.ts`:
```ts
import { expect, test } from 'vitest';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { existsSync, rmSync } from 'node:fs';
const run = promisify(execFile);
test('cli renders a sidecar to its wav source path', async () => {
  const out = 'fixtures/assets/audio/mus/fixture/synth.wav';
  rmSync(out, { force: true });
  await run('npx', ['tsx', 'src/cli.ts', 'fixtures/synth.asset.yaml'], { cwd: process.cwd() });
  expect(existsSync(out)).toBe(true);
}, 120_000);
```
(Ensure `fixtures/synth.asset.yaml`'s `source` resolves under `fixtures/`; adjust the fixture `source` to `assets/audio/mus/fixture/synth.wav` and have `loadStem` resolve `wavOutPath` relative to the sidecar dir — already does.)

- [ ] **Step 2: Run to verify it fails**

Run: `npm test -- cli`
Expected: FAIL (no cli.ts).

- [ ] **Step 3: Implement**

`src/cli.ts`:
```ts
import { writeFile, mkdir } from 'node:fs/promises';
import { dirname } from 'node:path';
import { glob } from 'node:fs/promises';
import { renderStem } from './render.js';
import { loadStem } from './sidecar.js';

async function main() {
  const args = process.argv.slice(2);
  const check = args.includes('--check');
  const all = args.includes('--all');
  const targets = args.filter(a => !a.startsWith('--'));
  let sidecars: string[] = targets;
  if (all) { sidecars = []; for await (const f of glob('content/**/*.asset.yaml')) sidecars.push(f); }

  let failures = 0;
  for (const sc of sidecars) {
    const stem = await loadStem(sc).catch(() => null);
    if (!stem) continue; // not a music_stem or no manifest → skip (L023 covers policy)
    const r = await renderStem(sc, { repoRoot: '.' });
    if (r.seam > 0.2) { console.error(`SEAM FAIL ${sc}: discontinuity ${r.seam.toFixed(3)}`); failures++; continue; }
    if (check) {
      // verify provenance derivation matches what the sidecar declares
      if (stem.sidecar.provenance?.source_tier !== r.provenance.source_tier) {
        console.error(`PROVENANCE MISMATCH ${sc}: sidecar=${stem.sidecar.provenance?.source_tier} derived=${r.provenance.source_tier}`); failures++;
      }
      console.log(`OK (check) ${sc}`);
    } else {
      await mkdir(dirname(r.wavOutPath), { recursive: true });
      await writeFile(r.wavOutPath, r.wav);
      console.log(`rendered ${sc} → ${r.wavOutPath} (seam ${r.seam.toFixed(3)}, ${r.provenance.source_tier})`);
    }
  }
  if (failures) { console.error(`${failures} stem(s) failed`); process.exit(1); }
}
main().catch(e => { console.error(e); process.exit(1); });
```
(If `node:fs/promises` `glob` is unavailable on the pinned Node, swap to the `glob` npm package — Node 26 in CI has it; note this in README.)

- [ ] **Step 4: Run to verify it passes**

Run: `npm test -- cli`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/strudel_render/src/cli.ts tools/strudel_render/test/cli.test.ts
git commit -m "feat(strudel-render): CLI (render / --check / --all)"
```

---

## Task 12: `validate_content.py` lint L023 (strudel-backed stem needs a render manifest)

**Files:**
- Modify: `tools/validate_content.py`
- Test: `tools/tests/` (match the repo's existing validator test layout — locate with `ls tools/*test* tools/**/test_*.py`)

- [ ] **Step 1: Find the validator's test file and lint-emit pattern**

Run: `grep -rn "L022\|def check_asset\|lints.append\|LintError\|def _lint" tools/validate_content.py | head`
Read the surrounding function to match the exact error-append idiom (rule-id string + rel path). Locate its tests: `grep -rln "L022\|validate_content" tools/ | grep -i test`.

- [ ] **Step 2: Write the failing test**

In the validator's test module, following its existing style, add a case: a `music_stem` sidecar whose `extra_sources` includes a `*.strudel` path but which has **no** sibling `*.render.yaml` must emit `L023`; one **with** the manifest must not.
```python
def test_L023_strudel_stem_requires_render_manifest(tmp_path):
    # build a minimal content tree with a music_stem sidecar + a .strudel extra_source,
    # no .render.yaml sibling → expect an "L023" error string in the report.
    ...  # mirror the existing L020/L022 test fixtures in this module
```

- [ ] **Step 3: Run to verify it fails**

Run: `uv run pytest tools -k L023 -q`
Expected: FAIL (no L023 emitted).

- [ ] **Step 4: Implement L023**

In the asset-sidecar lint pass (beside L020/L021/L022), for `class == music_stem`:
```python
# L023 — a stem authored as a Strudel pattern must ship a render manifest so the
# WAV `source` is reproducible (docs/superpowers/specs/2026-07-08-strudel-...).
extra = sidecar.get("extra_sources") or []
strudel = [s for s in extra if str(s).endswith(".strudel")]
if strudel:
    # manifest sits next to the sidecar: <name>.render.yaml
    manifest = sidecar_path.with_name(sidecar_path.name.replace(".asset.yaml", ".render.yaml"))
    if not manifest.exists():
        errors.append(f"L023 {rel_path}: music_stem has a .strudel extra_source but no "
                      f"render manifest at {manifest.name}")
```
Add `L023` to the module docstring rule table. Match the actual `errors`/`report` variable names found in Step 1.

- [ ] **Step 5: Run to verify it passes**

Run: `uv run pytest tools -k L023 -q && uv run tools/validate_content.py`
Expected: L023 test PASS; full validator still exits 0 on `content/` (no strudel stems yet, so L023 is inert until Task 13).

- [ ] **Step 6: Commit**

```bash
git add tools/validate_content.py tools/**/test_*validate*.py
git commit -m "feat(content-lint): L023 strudel-backed stem requires render manifest"
```

---

## Task 13: End-to-end — author + render the zone01 stem set; wire CI

**Files:**
- Create: `content/core/assets/audio/mus/zone01/{explore,tension,combat}.strudel`
- Create: `content/core/assets/audio/mus/zone01/{explore,tension,combat}.render.yaml`
- Modify: the three existing sidecars `content/core/assets/mus/zone01_{explore,tension,combat}.asset.yaml` (add `extra_sources` pointing at the `.strudel`)
- Modify: `scripts/content-build.sh` (render before emit)
- Modify: `.github/workflows/content-ci.yml` (add strudel-render job)

- [ ] **Step 1: Author the three tempo/key-locked patterns**

Write `.strudel` sources sharing bpm/key/length from their sidecars (`bpm: 96`, `d_dorian`, `length_bars: 96` per the existing tension sidecar). Explore = L1+L2 material, tension = L3, combat = L4 — each a self-contained loop. Keep them **pure synthesis** for the first drop (source_tier original). Example `tension.strudel`:
```
stack(
  note("d2 ~ a2 ~".slow(2)).s("sawtooth").lpf(500).gain(0.5),
  s("~").struct("t*8").bank("meridian_perc")  // omit for pure-synth first drop
).slow(96)
```
Add matching `<name>.render.yaml` (variant 0, tail_seconds 4.0, sample_banks []) for each.

- [ ] **Step 2: Point the sidecars at the sources**

Add to each `zone01_*.asset.yaml`:
```yaml
extra_sources: [assets/audio/mus/zone01/<name>.strudel]
```
(The `source:` WAV path already exists in these placeholder sidecars.)

- [ ] **Step 3: Render locally and verify they load**

Run: `cd tools/strudel_render && npm ci && npx tsx src/cli.ts ../../content/core/assets/mus/zone01_explore.asset.yaml`
Expected: WAV written to the sidecar's `source` path; seam < 0.2; provenance `original`. Repeat for tension/combat, then run `uv run tools/validate_content.py` from repo root → exit 0 (L023 satisfied, loudness present).

- [ ] **Step 4: Add the render step to `scripts/content-build.sh`**

Before the `# --- 2. Run the full content build.` section (so WAVs exist before `mcc emit-pck` packs them), insert:
```bash
# --- 1b. Render Strudel-authored stems to their WAV sources (generated artifacts). ---
if [ -d tools/strudel_render ] && command -v node >/dev/null 2>&1; then
  log "Rendering Strudel stems (tools/strudel_render)"
  ( cd tools/strudel_render && npm ci --silent && npx tsx src/cli.ts --all )
  ok "Strudel stems rendered"
else
  warn "tools/strudel_render or node unavailable — skipping stem render (assumes WAVs present)"
fi
```

- [ ] **Step 5: Add a CI job to `content-ci.yml`**

Add under `jobs:` (runs the renderer's own tests + a deterministic `--check`):
```yaml
  strudel-render:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: "22" }
      - name: Install + test renderer
        working-directory: tools/strudel_render
        run: |
          npm ci
          npx playwright install --with-deps chromium || true   # only needed if Playwright backend selected
          npm test
      - name: Render all stems (deterministic check)
        working-directory: tools/strudel_render
        run: npx tsx src/cli.ts --all --check
```

- [ ] **Step 6: Full local verification**

Run: `NO_COLOR=1 ./scripts/content-build.sh` (from repo root)
Expected: renders stems, then mcc validate→link→emit succeeds, determinism gate passes (rendered WAVs are byte-identical across the two builds — if not, revisit Task 10's backend determinism). Confirm `build/content-out/` artifacts produced.

- [ ] **Step 7: Commit**

```bash
git add content/core/assets scripts/content-build.sh .github/workflows/content-ci.yml
git commit -m "feat(music): render zone01 adaptive stem set from Strudel + wire CI"
```

---

## Self-Review

**Spec coverage:**
- §3 firewall / dev-only tool → Tasks 1–2 (isolated Node tool), never added to client/server builds. ✓
- §3 backend spike (pure-Node vs Playwright) → Task 1. ✓
- §4 pipeline (read sidecar+manifest → render → wrap → wav; render before validate/pack) → Tasks 4, 9, 13. ✓
- §5 determinism + variant + pinned versions → Tasks 8 (deps in cache key), 10 (byte-identical gate), 1 (1.2.5 pin). ✓
- §6 seamless loop render-and-wrap + seam check → Task 7, enforced in Task 9/11. ✓
- §7 provenance (original|cc0, never ai) + sample firewall → Tasks 5, 6. ✓
- §8 integration with `meridian/asset@1`, no schema change, authoritative metadata, validator round-trip → Tasks 4, 12. ✓
- §9 tests (determinism, length, loop, allowlist, provenance, e2e) → Tasks 3–13. ✓
- §10 risks (backend fidelity gated; CI cost via cache) → Tasks 1, 8. ✓
- §11 M0 fit (zone01 set as pipeline proof) → Task 13. ✓

**Placeholder scan:** No "TBD/TODO/implement later". Task 1 is a spike whose deliverable is a verified backend module with concrete probe scripts and a decision gate — not a placeholder. Task 12 intentionally defers exact variable names to a `grep` step because it edits an existing 600-line file whose idioms must be matched in place; the rule id, condition, and message are fully specified.

**Type consistency:** `RenderParams`, `StereoBuffer`, `RenderBackend` (Task 1) used verbatim in Tasks 3–11. `loadStem`→`Stem` (Task 4) consumed by `renderStem` (Task 9) and `cli.ts` (Task 11). `usedSampleBanks`/`enforceAllowlist`/`BankRecord` (Task 5) → `deriveProvenance` (Task 6) → `renderStem` (Task 9). `wrapTail`/`seamDiscontinuity` (Task 7) → `renderStem` (Task 9). `cacheKey`/`cacheGet`/`cachePut` (Task 8) → `renderStem` (Task 9). Consistent.

**Known follow-ups (out of scope, flagged):** stinger/ambient-bed rendering; CC-BY sample support (attribution aggregation); mcc packing of generated WAVs into the golden corpus (currently WAV sources are not in the golden set — confirm mcc tolerates generated `source` files, or add a render step ahead of the golden job too).
