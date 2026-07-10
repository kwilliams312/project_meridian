// Browser-side render entry. esbuild bundles this to an IIFE that the Playwright
// backend injects into a fresh headless-Chromium page (one render per page — a
// fresh JS realm keeps superdough's cached global audio nodes from leaking
// across renders and silencing later ones).
//
// Exposes window.__meridianRender(params) -> [leftBase64, rightBase64] of raw
// little-endian Float32 PCM (base64 keeps the CDP payload small vs. JSON arrays).
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import { evaluate } from '@strudel/transpiler';
import { superdough, initAudio, registerSynthSounds } from 'superdough';

async function renderStrudel({ code, cps, sampleRate, totalSeconds }) {
  const frames = Math.ceil(totalSeconds * sampleRate);
  const octx = new OfflineAudioContext(2, frames, sampleRate);
  // An OfflineAudioContext cannot be resume()'d before startRendering, but
  // superdough's initAudio() calls resume() unconditionally — no-op it.
  octx.resume = () => Promise.resolve();
  // superdough.getAudioContext() lazily does `new AudioContext()`; hand back our
  // offline ctx so every node superdough builds lives on the render graph.
  window.AudioContext = function () { return octx; };

  await core.evalScope(core, mini, tonal, core.controls);
  // Point mini-notation at the same Pattern copy note()/s() use (dist quirk).
  core.setStringParser(mini.mini);
  await initAudio();          // loads AudioWorklet effects/synths into octx
  registerSynthSounds();      // registers sine/sawtooth/supersaw/... sources

  const { pattern } = await evaluate(code);
  const cycles = totalSeconds * cps;
  const haps = pattern.queryArc(0, cycles).filter((h) => (h.hasOnset ? h.hasOnset() : true));
  for (const h of haps) {
    const begin = Number(h.whole.begin.valueOf()) / cps;
    const dur = (Number(h.whole.end.valueOf()) - Number(h.whole.begin.valueOf())) / cps;
    // superdough refuses t < ctx.currentTime (0 for offline); a tiny lead keeps
    // the very first onset schedulable without shifting audible timing.
    await superdough(h.value, begin + 0.05, dur, cps);
  }

  const buf = await octx.startRendering();
  const toB64 = (f32) => {
    const bytes = new Uint8Array(f32.buffer, f32.byteOffset, f32.byteLength);
    let bin = '';
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    }
    return btoa(bin);
  };
  return [toB64(buf.getChannelData(0)), toB64(buf.getChannelData(1))];
}

window.__meridianRender = renderStrudel;
