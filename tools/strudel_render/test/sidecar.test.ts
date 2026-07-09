import { expect, test } from 'vitest';
import { selectBackend } from '../src/backend/index.js';
import { loadStem } from '../src/sidecar.js';

test('selectBackend returns a RenderBackend', () => {
  expect(typeof selectBackend().render).toBe('function');
});

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
