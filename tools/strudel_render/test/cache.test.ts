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
