import type { RenderBackend } from '../types.js';
// Backend selected by the Task 1 spike (see spike/DECISION.md): headless Chromium.
// The render contract: render() returns `lengthBars` of loop audio PLUS
// `tailSeconds` of FX tail, so the loop-wrap step (Task 7) has a tail to fold.
import { backend } from './playwright-backend.js';

export function selectBackend(): RenderBackend {
  return backend;
}
