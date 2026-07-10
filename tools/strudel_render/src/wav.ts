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
