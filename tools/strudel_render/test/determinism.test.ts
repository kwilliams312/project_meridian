import { expect, test } from 'vitest';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { renderStem } from '../src/render.js';

test('same source renders byte-identical WAV across runs (cache cleared)', async () => {
  // Use a private cache dir so clearing it can't race another test file's
  // cachePut on the shared '.render-cache' (see test/cache-isolation.test.ts).
  const cacheDir = await mkdtemp(join(tmpdir(), 'strudel-determinism-'));
  try {
    await rm(cacheDir, { recursive: true, force: true });
    const a = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.', cacheDir });
    await rm(cacheDir, { recursive: true, force: true });
    const b = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.', cacheDir });
    expect(Buffer.compare(a.wav, b.wav)).toBe(0);
  } finally {
    await rm(cacheDir, { recursive: true, force: true });
  }
}, 120_000);
