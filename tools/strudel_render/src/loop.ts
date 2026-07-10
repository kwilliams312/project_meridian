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
  return jump / (rms * 3); // normalize; hard cut ≫ continuous
}
