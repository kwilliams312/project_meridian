import { createServer, type Server } from 'node:http';
import { fileURLToPath } from 'node:url';
import { chromium, type Browser } from 'playwright';
import * as esbuild from 'esbuild';
import type { RenderBackend, RenderParams, StereoBuffer } from '../types.js';

/**
 * Headless-Chromium render backend (spike-selected — see spike/DECISION.md).
 *
 * Strudel's `@strudel/*` pattern engine + superdough sound engine run in-page
 * against an OfflineAudioContext. This is the only backend that loads
 * superdough's AudioWorklet synths/effects (supersaw, coarse, crush, ladder);
 * pure Node (node-web-audio-api) cannot resolve their data-URL worklet modules.
 *
 * Each render runs in a **fresh browser context** so superdough's cached global
 * audio nodes never leak across renders (which would silence later ones).
 */

type RenderMessage = [string, string]; // [leftBase64, rightBase64]

const ENTRY = fileURLToPath(new URL('./browser-entry.mjs', import.meta.url));

let bundlePromise: Promise<string> | undefined;
let browserPromise: Promise<Browser> | undefined;
let serverPromise: Promise<{ server: Server; port: number }> | undefined;

async function getBundle(): Promise<string> {
  if (!bundlePromise) {
    bundlePromise = esbuild
      .build({
        entryPoints: [ENTRY],
        bundle: true,
        format: 'iife',
        platform: 'browser',
        write: false,
        logLevel: 'silent',
      })
      .then((r) => r.outputFiles[0].text);
  }
  return bundlePromise;
}

async function getBrowser(): Promise<Browser> {
  if (!browserPromise) browserPromise = chromium.launch();
  return browserPromise;
}

async function getServer(): Promise<{ server: Server; port: number }> {
  if (!serverPromise) {
    serverPromise = new Promise((resolve) => {
      // A localhost origin is a Chromium "secure context" — required for AudioWorklet.
      const server = createServer((_req, res) => {
        res.setHeader('content-type', 'text/html');
        res.end('<!doctype html><html><head><meta charset="utf-8"></head><body></body></html>');
      });
      server.listen(0, '127.0.0.1', () => {
        const addr = server.address();
        const port = typeof addr === 'object' && addr ? addr.port : 0;
        resolve({ server, port });
      });
    });
  }
  return serverPromise;
}

/** Beats per bar from a "n/d" time signature (numerator). */
function beatsPerBar(timeSignature: string): number {
  const n = Number(timeSignature.split('/')[0]);
  return Number.isFinite(n) && n > 0 ? n : 4;
}

function base64ToFloat32(b64: string): Float32Array {
  const buf = Buffer.from(b64, 'base64');
  // Copy into an aligned ArrayBuffer so the Float32Array view is valid.
  const out = new Float32Array(buf.byteLength / 4);
  for (let i = 0; i < out.length; i++) out[i] = buf.readFloatLE(i * 4);
  return out;
}

async function render(params: RenderParams): Promise<StereoBuffer> {
  const { code, bpm, timeSignature, lengthBars, tailSeconds, sampleRate } = params;
  // cps: cycles per second. 1 cycle == 1 bar. cps = (bpm/60) / beatsPerBar.
  const cps = bpm / 60 / beatsPerBar(timeSignature);
  const loopSeconds = lengthBars / cps; // lengthBars cycles at cps
  const totalSeconds = loopSeconds + tailSeconds;

  const [bundle, browser, { port }] = await Promise.all([getBundle(), getBrowser(), getServer()]);
  const context = await browser.newContext();
  try {
    const page = await context.newPage();
    await page.goto(`http://localhost:${port}/`);
    await page.addScriptTag({ content: bundle });
    const [leftB64, rightB64] = (await page.evaluate(
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      async (p: any) => (globalThis as any).__meridianRender(p),
      { code, cps, sampleRate, totalSeconds },
    )) as RenderMessage;
    return { left: base64ToFloat32(leftB64), right: base64ToFloat32(rightB64), sampleRate };
  } finally {
    await context.close();
  }
}

/** Release the shared browser + server (call once when a batch of renders is done). */
export async function shutdown(): Promise<void> {
  if (browserPromise) {
    const b = await browserPromise;
    await b.close();
    browserPromise = undefined;
  }
  if (serverPromise) {
    const { server } = await serverPromise;
    await new Promise<void>((resolve) => server.close(() => resolve()));
    serverPromise = undefined;
  }
}

// Ensure a short-lived process (CLI, single test) can exit without a dangling browser.
process.once('beforeExit', () => {
  void shutdown();
});

export const backend: RenderBackend = { render };
