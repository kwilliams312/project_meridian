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
