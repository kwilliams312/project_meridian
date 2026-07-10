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
