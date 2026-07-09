import { expect, test } from 'vitest';
import { renderStem } from '../src/render.js';

test('renderStem produces a non-silent WAV of the declared length and clean provenance', async () => {
  const r = await renderStem('fixtures/synth.asset.yaml', { repoRoot: '.' });
  // 4 bars @ 120 bpm / (4 beats/bar) = 8s → 8*44100 frames
  const frames = (r.wav.length - 44) / 4;
  expect(frames).toBeGreaterThan(44100 * 7); // ~8s, allow rounding
  expect(frames).toBeLessThan(44100 * 9);
  expect(r.provenance.source_tier).toBe('original');
  expect(r.seam).toBeLessThan(0.2); // seamless
  const pcm = r.wav.subarray(44);
  expect(pcm.some((_, i) => i % 2 === 0 && Math.abs(pcm.readInt16LE(i)) > 100)).toBe(true); // non-silent
}, 60_000);
