import { expect, test } from 'vitest';
import { deriveProvenance } from '../src/provenance.js';
test('pure synthesis → source_tier original', () => {
  const p = deriveProvenance([], ['meridian-contributors']);
  expect(p).toEqual({ source_tier: 'original', authors: ['meridian-contributors'] });
});
test('CC0 sample use → source_tier cc0 with origin + verified date', () => {
  const p = deriveProvenance(
    [{ origin_url: 'https://x.invalid/b', license: 'CC0-1.0', license_verified_on: '2026-07-08' }],
    ['meridian-contributors']);
  expect(p.source_tier).toBe('cc0');
  expect(p.origin_url).toBe('https://x.invalid/b');
  expect(p.license_verified_on).toBe('2026-07-08');
});
