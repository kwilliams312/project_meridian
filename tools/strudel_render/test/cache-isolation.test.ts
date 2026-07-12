import { expect, test } from 'vitest';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { cacheGet, cachePut } from '../src/cache.js';

// Regression guard for the CI flake where determinism.test.ts's cache-clear
// (rm '.render-cache') raced render.e2e.test.ts's cachePut (mkdir/writeFile on
// the same '.render-cache'), intermittently throwing ENOENT. The fix gives each
// caller its own cache directory; these tests lock in that isolation contract.

test('cachePut/cacheGet honor a per-call directory so callers cannot collide', async () => {
  const dirA = await mkdtemp(join(tmpdir(), 'strudel-cache-a-'));
  const dirB = await mkdtemp(join(tmpdir(), 'strudel-cache-b-'));
  try {
    const key = 'a'.repeat(64);
    const wav = Buffer.from('isolated-payload');
    await cachePut(key, wav, dirA);
    expect(await cacheGet(key, dirA)).toEqual(wav); // visible in its own dir
    expect(await cacheGet(key, dirB)).toBeNull(); // NOT visible in an unrelated dir
  } finally {
    await rm(dirA, { recursive: true, force: true });
    await rm(dirB, { recursive: true, force: true });
  }
});

test('clearing one cache dir never breaks concurrent writes to another', async () => {
  const writeDir = await mkdtemp(join(tmpdir(), 'strudel-write-'));
  const clearDir = await mkdtemp(join(tmpdir(), 'strudel-clear-'));
  try {
    let stop = false;
    // Mirror determinism.test.ts hammering rm on ITS cache dir...
    const clearer = (async () => {
      while (!stop) {
        await rm(clearDir, { recursive: true, force: true });
      }
    })();
    // ...while render.e2e.test.ts writes to ITS own cache dir. Must never throw.
    const wav = Buffer.alloc(1024, 7);
    for (let i = 0; i < 200; i++) {
      await cachePut(`k${i}`.padEnd(64, '0'), wav, writeDir);
    }
    stop = true;
    await clearer;
  } finally {
    await rm(writeDir, { recursive: true, force: true });
    await rm(clearDir, { recursive: true, force: true });
  }
});

test('an explicit cache dir is never the shared repo-level .render-cache', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'strudel-explicit-'));
  try {
    expect(resolve(dir)).not.toBe(resolve('.render-cache'));
    const key = 'b'.repeat(64);
    await cachePut(key, Buffer.from('x'), dir);
    // The shared default dir must be untouched by an explicit-dir write.
    expect(await cacheGet(key)).toBeNull();
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});
