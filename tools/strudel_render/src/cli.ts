import { writeFile, mkdir, glob } from 'node:fs/promises';
import { dirname } from 'node:path';
import { renderStem } from './render.js';
import { loadStem } from './sidecar.js';
import { shutdown } from './backend/playwright-backend.js';

/** Render (or --check) each music_stem sidecar to its declared WAV `source` path. */
async function main(): Promise<number> {
  const args = process.argv.slice(2);
  const check = args.includes('--check');
  const all = args.includes('--all');
  const targets = args.filter((a) => !a.startsWith('--'));

  let sidecars: string[] = targets;
  if (all) {
    sidecars = [];
    for await (const f of glob('content/**/*.asset.yaml')) sidecars.push(f);
  }

  let failures = 0;
  for (const sc of sidecars) {
    // Skip non-music_stem sidecars and those without a render manifest.
    // loadStem throws for both; L023 enforces the manifest policy separately.
    const stem = await loadStem(sc).catch(() => null);
    if (!stem) continue;

    const r = await renderStem(sc, { repoRoot: '.', backendName: 'playwright' });
    if (r.seam > 0.2) {
      console.error(`SEAM FAIL ${sc}: discontinuity ${r.seam.toFixed(3)}`);
      failures++;
      continue;
    }

    if (check) {
      const declared = stem.sidecar.provenance?.source_tier;
      if (declared !== r.provenance.source_tier) {
        console.error(
          `PROVENANCE MISMATCH ${sc}: sidecar=${declared} derived=${r.provenance.source_tier}`,
        );
        failures++;
      }
      console.log(`OK (check) ${sc}`);
    } else {
      await mkdir(dirname(r.wavOutPath), { recursive: true });
      await writeFile(r.wavOutPath, r.wav);
      console.log(
        `rendered ${sc} → ${r.wavOutPath} (seam ${r.seam.toFixed(3)}, ${r.provenance.source_tier})`,
      );
    }
  }

  if (failures) console.error(`${failures} stem(s) failed`);
  return failures ? 1 : 0;
}

main()
  .catch((e) => {
    console.error(e);
    return 1;
  })
  // Release the shared headless browser + HTTP server the Playwright backend keeps
  // alive across renders; without this the open handles pin the event loop open and
  // the process never exits (beforeExit never fires).
  .finally(async () => {
    await shutdown();
  })
  .then((code) => process.exit(code ?? 0));
