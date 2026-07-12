import { expect, test } from 'vitest';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { renderStem } from '../src/render.js';

test('renderStem produces a non-silent WAV of the declared length and clean provenance', async () => {
  // Private cache dir keeps this hermetic and off the shared '.render-cache'.
  const cacheDir = await mkdtemp(join(tmpdir(), 'strudel-e2e-'));
  try {
  const r = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.', cacheDir });
  // 4 bars @ 120 bpm / (4 beats/bar) = 8s → 8*44100 frames
  const frames = (r.wav.length - 44) / 4;
  expect(frames).toBeGreaterThan(44100 * 7); // ~8s, allow rounding
  expect(frames).toBeLessThan(44100 * 9);
  expect(r.provenance.source_tier).toBe('original');
  expect(r.seam).toBeLessThan(0.2); // seamless
  const pcm = r.wav.subarray(44);
  expect(pcm.some((_, i) => i % 2 === 0 && Math.abs(pcm.readInt16LE(i)) > 100)).toBe(true); // non-silent
  } finally {
    await rm(cacheDir, { recursive: true, force: true });
  }
}, 60_000);
