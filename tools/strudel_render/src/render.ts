import { createRequire } from 'node:module';
import { loadStem, beatsPerBar } from './sidecar.js';
import { selectBackend } from './backend/index.js';
import { usedSampleBanks, enforceAllowlist } from './samples.js';
import { deriveProvenance, type Provenance } from './provenance.js';
import { wrapTail, seamDiscontinuity } from './loop.js';
import { encodeWav } from './wav.js';
import { cacheKey, cacheGet, cachePut } from './cache.js';

const require = createRequire(import.meta.url);

/** Pinned dependency versions folded into the cache key (render reproducibility). */
function depVersions(backend: string): Record<string, string> {
  const v = (n: string): string => require(`${n}/package.json`).version;
  return { '@strudel/core': v('@strudel/core'), superdough: v('superdough'), backend };
}

export interface RenderResult {
  wav: Buffer;
  provenance: Provenance;
  seam: number;
  wavOutPath: string;
}

export async function renderStem(
  sidecarPath: string,
  opts: { repoRoot: string; backendName?: string; cacheDir?: string },
): Promise<RenderResult> {
  const stem = await loadStem(sidecarPath);
  const banks = usedSampleBanks(stem.params.code);
  const allowlistRoot = opts.repoRoot === '.' ? 'tools/strudel_render' : `${opts.repoRoot}/tools/strudel_render`;
  const records = await enforceAllowlist(banks, allowlistRoot);
  const provenance = deriveProvenance(records, stem.sidecar.provenance?.authors ?? ['meridian-contributors']);

  const backendName = opts.backendName ?? 'playwright';
  const key = cacheKey(stem.params, depVersions(backendName));
  let wav = await cacheGet(key, opts.cacheDir);
  let seam = 0;
  if (!wav) {
    const beats = beatsPerBar(stem.params.timeSignature);
    const raw = await selectBackend().render(stem.params); // renders lengthBars + tailSeconds
    const loopN = Math.round(
      ((stem.params.lengthBars * beats) / (stem.params.bpm / 60)) * stem.params.sampleRate,
    );
    const looped = stem.sidecar.music.loop === false ? raw : wrapTail(raw, loopN);
    seam = seamDiscontinuity(looped);
    wav = encodeWav(looped);
    await cachePut(key, wav, opts.cacheDir);
  }
  return { wav, provenance, seam, wavOutPath: stem.wavOutPath };
}
