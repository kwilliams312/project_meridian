// Spike probe: does Playwright + headless Chromium render Strudel (incl. superdough
// AudioWorklet synths/effects) to non-silent audio via an in-page OfflineAudioContext?
//
// Self-contained (no live strudel.cc dependency): the pinned @strudel/* + superdough
// deps are esbuild-bundled to an in-page IIFE that exposes window.__meridianRender.
// This is the exact mechanism the selected backend (src/backend/playwright-backend.ts)
// uses. Run: npx playwright install chromium && npm run spike:pw
import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ENTRY = fileURLToPath(new URL('../src/backend/browser-entry.mjs', import.meta.url));
const { outputFiles } = await esbuild.build({
  entryPoints: [ENTRY], bundle: true, format: 'iife', platform: 'browser', write: false, logLevel: 'silent',
});
const bundle = outputFiles[0].text;

// localhost origin == Chromium "secure context", required for AudioWorklet.
const server = createServer((_req, res) => { res.setHeader('content-type', 'text/html'); res.end('<!doctype html><html><body></body></html>'); });
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const browser = await chromium.launch();

async function render(code) {
  // Fresh context per render → clean superdough global state (else later renders go silent).
  const ctx = await browser.newContext();
  try {
    const page = await ctx.newPage();
    await page.goto(`http://localhost:${port}/`);
    await page.addScriptTag({ content: bundle });
    const [l] = await page.evaluate(
      async (code) => window.__meridianRender({ code, cps: 0.5, sampleRate: 44100, totalSeconds: 4 }),
      code,
    );
    const buf = Buffer.from(l, 'base64');
    const f = new Float32Array(buf.buffer, buf.byteOffset, buf.length / 4);
    let peak = 0;
    for (let i = 0; i < f.length; i++) peak = Math.max(peak, Math.abs(f[i]));
    return { frames: f.length, peak: Number(peak.toFixed(5)), nonSilent: peak > 1e-4 };
  } finally {
    await ctx.close();
  }
}

// A pure-oscillator pattern and worklet-using patterns (supersaw/coarse/crush/ladder).
const patterns = {
  pure: `note("c4 e4 g4").s("sine")`,
  worklet_supersaw: `note("c3 e3").s("supersaw")`,
  worklet_coarse: `note("c3").s("sawtooth").coarse(4)`,
  worklet_crush: `note("c3").s("sawtooth").crush(4)`,
  worklet_ladder: `note("c3 e3").s("sawtooth").lpf(600).ftype("ladder")`,
};
for (const [name, code] of Object.entries(patterns)) {
  console.log(name, JSON.stringify(await render(code)));
}

await browser.close();
server.close();
