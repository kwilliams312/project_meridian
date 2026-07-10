import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import yaml from 'js-yaml';

export const SYNTH_SOURCES = new Set([
  'sine', 'sawtooth', 'saw', 'square', 'triangle', 'tri', 'pulse',
  'white', 'pink', 'brown', 'z_sawtooth', 'supersaw',
]);

export interface BankRecord { origin_url: string; license: string; license_verified_on: string; }

// Static scan: collect string args to s(...) / sound(...). A bank ref is the part before ':'.
export function usedSampleBanks(code: string): string[] {
  const banks = new Set<string>();
  const re = /\b(?:s|sound)\s*\(\s*(["'`])([^"'`]+)\1/g;
  for (let m; (m = re.exec(code)); ) {
    for (const tok of m[2].split(/\s+/)) {
      const name = tok.split(':')[0].replace(/[<>[\]!@*/?.]/g, '').trim();
      if (name && !SYNTH_SOURCES.has(name) && !/^~?\d+$/.test(name)) banks.add(name);
    }
  }
  return [...banks];
}

export async function enforceAllowlist(banks: string[], repoRootForAllowlist: string): Promise<BankRecord[]> {
  if (banks.length === 0) return [];
  const allowPath = resolve(repoRootForAllowlist, 'sample-banks.allowlist.yaml');
  const doc: any = yaml.load(await readFile(allowPath, 'utf8'));
  const table = doc?.banks ?? {};
  const out: BankRecord[] = [];
  for (const b of banks) {
    if (!table[b]) throw new Error(`sample bank "${b}" is not on the CC0 allowlist (${allowPath})`);
    out.push(table[b]);
  }
  return out;
}
