import { expect, test } from 'vitest';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { existsSync, rmSync } from 'node:fs';

const run = promisify(execFile);

// loadStem resolves the WAV output as resolve(sidecarDir, basename(source)),
// so the fixture sidecar's `source: assets/audio/mus/fixture/synth.wav`
// renders to fixtures/synth.wav (directory component stripped by basename).
const OUT = 'fixtures/synth.wav';

test('cli renders a music_stem sidecar to its wav source path', async () => {
  rmSync(OUT, { force: true });
  await run('npx', ['tsx', 'src/cli.ts', 'fixtures/synth.asset.yaml'], { cwd: process.cwd() });
  expect(existsSync(OUT)).toBe(true);
}, 120_000);

test('cli --check verifies without writing the wav', async () => {
  rmSync(OUT, { force: true });
  const { stdout } = await run('npx', ['tsx', 'src/cli.ts', '--check', 'fixtures/synth.asset.yaml'], {
    cwd: process.cwd(),
  });
  expect(stdout).toContain('OK (check)');
  expect(existsSync(OUT)).toBe(false);
}, 120_000);

test('cli skips a music_stem sidecar with no render manifest', async () => {
  // no-manifest.asset.yaml has no sibling .render.yaml → loadStem throws → skipped, exit 0.
  const { stdout } = await run('npx', ['tsx', 'src/cli.ts', 'fixtures/no-manifest.asset.yaml'], {
    cwd: process.cwd(),
  });
  expect(stdout).not.toContain('rendered');
}, 120_000);
