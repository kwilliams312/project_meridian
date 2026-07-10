// Spike probe (one render per process — matches superdough's global-node caching model).
// Usage: node spike/probe-node.mjs '<strudel code>'
// Prints one JSON line: { haps, scheduled, nonSilent, peak, schedErr }.
import * as nwa from 'node-web-audio-api';

// superdough references bare globals (`new AudioContext()`, `new AudioWorkletNode(...)`).
for (const [k, v] of Object.entries(nwa)) {
  if (typeof v === 'function' && !(k in globalThis)) globalThis[k] = v;
}

const code = process.argv[2] ?? `note("c4 e4 g4").s("sine")`;
const SR = 44100;
const cycles = 2;
const cps = 0.5; // 1 cycle = 2s

const octx = new nwa.OfflineAudioContext({ numberOfChannels: 2, length: SR * (cycles / cps), sampleRate: SR });
// superdough.getAudioContext() lazily does `new AudioContext()`; hand back our offline ctx.
globalThis.AudioContext = function () { return octx; };
globalThis.OfflineAudioContext = nwa.OfflineAudioContext;

const superdoughMod = await import('superdough');
const { superdough, initAudio, registerSynthSounds } = superdoughMod;

const core = await import('@strudel/core');
const mini = await import('@strudel/mini');
const tonal = await import('@strudel/tonal');
const { evaluate } = await import('@strudel/transpiler');
const { evalScope, controls } = core;

await evalScope(core, mini, tonal, controls);
core.setStringParser(mini.mini);

let workletError = null;
try { await initAudio(); } catch (e) { workletError = e?.message ?? String(e); }
try { registerSynthSounds?.(); } catch (e) { workletError = (workletError ? workletError + ' | ' : '') + 'registerSynthSounds: ' + (e?.message ?? String(e)); }

const { pattern } = await evaluate(code);
const haps = pattern.queryArc(0, cycles).filter((h) => h.hasOnset?.() ?? true);
let scheduled = 0, schedErr = null;
for (const h of haps) {
  const t = Number(h.whole.begin.valueOf()) / cps;
  const dur = (Number(h.whole.end.valueOf()) - Number(h.whole.begin.valueOf())) / cps;
  try { await superdough(h.value, t + 0.05, dur, cps); scheduled++; }
  catch (e) { schedErr = e?.message ?? String(e); }
}

let nonSilent = false, peak = 0;
try {
  const buf = await octx.startRendering();
  const ch = buf.getChannelData(0);
  for (let i = 0; i < ch.length; i++) { const a = Math.abs(ch[i]); if (a > peak) peak = a; }
  nonSilent = peak > 1e-4;
} catch (e) { schedErr = (schedErr ? schedErr + ' | ' : '') + 'render: ' + (e?.message ?? String(e)); }

console.log(JSON.stringify({ code, haps: haps.length, scheduled, nonSilent, peak: Number(peak.toFixed(6)), schedErr, workletError }));
