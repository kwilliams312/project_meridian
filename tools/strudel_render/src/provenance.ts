import type { BankRecord } from './samples.js';
export interface Provenance {
  source_tier: 'original' | 'cc0';
  authors: string[];
  origin_url?: string;
  license_verified_on?: string;
}
export function deriveProvenance(banks: BankRecord[], authors: string[]): Provenance {
  if (banks.length === 0) return { source_tier: 'original', authors };
  // Any CC0 sample use → cc0 tier. Use the first bank's origin/date (schema requires the fields;
  // multi-bank aggregation into attribution is a follow-up if CC-BY is ever allowed).
  const b = banks[0];
  return { source_tier: 'cc0', authors, origin_url: b.origin_url, license_verified_on: b.license_verified_on };
}
