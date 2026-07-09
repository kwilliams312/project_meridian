import { expect, test } from 'vitest';
import { rm } from 'node:fs/promises';
import { renderStem } from '../src/render.js';

test('same source renders byte-identical WAV across runs (cache cleared)', async () => {
  await rm('.render-cache', { recursive: true, force: true });
  const a = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.' });
  await rm('.render-cache', { recursive: true, force: true });
  const b = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.' });
  expect(Buffer.compare(a.wav, b.wav)).toBe(0);
}, 120_000);
