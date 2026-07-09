import { expect, test } from 'vitest';
import { usedSampleBanks, enforceAllowlist } from '../src/samples.js';

test('usedSampleBanks finds s()/sound() bank names', () => {
  expect(usedSampleBanks(`note("c e g").s("sine")`)).toEqual([]); // synth waveforms are not banks
  expect(usedSampleBanks(`s("bd sd bd sd")`).sort()).toEqual(['bd', 'sd']);
  expect(usedSampleBanks(`sound("meridian_perc:2")`)).toEqual(['meridian_perc']);
});

test('enforceAllowlist rejects non-allowlisted banks', async () => {
  // The allowlist lives at the tool root; use '.' as the root and assert a bank
  // clearly absent from the CC0 allowlist ("bd") is rejected.
  await expect(enforceAllowlist(['bd'], '.')).rejects.toThrow(/not on the CC0 allowlist/i);
});

test('enforceAllowlist accepts allowlisted banks and returns their records', async () => {
  const recs = await enforceAllowlist(['meridian_perc'], '.');
  expect(recs[0].license).toBe('CC0-1.0');
});
