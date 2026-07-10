import { readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, resolve, basename } from 'node:path';
import yaml from 'js-yaml';
import type { RenderParams } from './types.js';

export interface RenderManifest { schema: string; pattern: string; variant: number; tail_seconds: number; sample_banks: string[]; }
export interface Stem { id: string; params: RenderParams; manifest: RenderManifest; codePath: string; wavOutPath: string; sidecar: any; sidecarPath: string; }

function beatsPerBar(sig: string): number { return Number(sig.split('/')[0]); }

/** Nearest ancestor dir containing pack.yaml — `source`/`pattern` are pack-root-relative. */
function findPackRoot(startDir: string): string {
  let d = resolve(startDir);
  for (;;) {
    if (existsSync(resolve(d, 'pack.yaml'))) return d;
    const parent = dirname(d);
    if (parent === d) throw new Error(`no pack.yaml found above ${startDir}; source/pattern paths are pack-root-relative`);
    d = parent;
  }
}

export async function loadStem(sidecarPath: string): Promise<Stem> {
  const sidecar: any = yaml.load(await readFile(sidecarPath, 'utf8'));
  if (sidecar?.class !== 'music_stem') throw new Error(`${sidecarPath}: not a music_stem`);
  const dir = dirname(sidecarPath);
  const manifestPath = resolve(dir, basename(sidecarPath).replace(/\.asset\.ya?ml$/, '.render.yaml'));
  let manifest: RenderManifest;
  try { manifest = yaml.load(await readFile(manifestPath, 'utf8')) as RenderManifest; }
  catch { throw new Error(`${sidecarPath}: missing render manifest at ${manifestPath}`); }
  const packRoot = findPackRoot(dir);
  const codePath = resolve(packRoot, manifest.pattern);
  const code = await readFile(codePath, 'utf8');
  const m = sidecar.music;
  const params: RenderParams = {
    code, bpm: m.bpm, timeSignature: m.time_signature, lengthBars: m.length_bars,
    key: m.key, variant: manifest.variant ?? 0, tailSeconds: manifest.tail_seconds ?? 0, sampleRate: 44100,
  };
  void beatsPerBar; // used by backend for cps; exported for reuse
  const wavOutPath = resolve(packRoot, sidecar.source);
  return { id: sidecar.id, params, manifest, codePath, wavOutPath, sidecar, sidecarPath };
}
export { beatsPerBar };
