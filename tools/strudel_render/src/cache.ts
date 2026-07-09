import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import type { RenderParams } from './types.js';

export function cacheKey(p: RenderParams, deps: Record<string, string>): string {
  const h = createHash('sha256');
  h.update(JSON.stringify({
    code: p.code, bpm: p.bpm, sig: p.timeSignature, bars: p.lengthBars,
    key: p.key ?? null, variant: p.variant, tail: p.tailSeconds, sr: p.sampleRate,
    deps: Object.keys(deps).sort().map(k => `${k}@${deps[k]}`),
  }));
  return h.digest('hex');
}
const DIR = resolve('.render-cache');
export async function cacheGet(key: string): Promise<Buffer | null> {
  try { return await readFile(resolve(DIR, `${key}.wav`)); } catch { return null; }
}
export async function cachePut(key: string, wav: Buffer): Promise<void> {
  await mkdir(DIR, { recursive: true }); await writeFile(resolve(DIR, `${key}.wav`), wav);
}
