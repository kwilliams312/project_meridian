/** A rendered stereo audio buffer of deinterleaved Float32 samples. */
export interface StereoBuffer {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
}

/** Parameters that fully determine one deterministic stem render. */
export interface RenderParams {
  code: string;
  bpm: number;
  timeSignature: string;
  lengthBars: number;
  key?: string;
  variant: number;
  tailSeconds: number;
  sampleRate: number;
}

/**
 * A render backend turns a Strudel pattern + params into audio.
 *
 * Contract: `render` produces `lengthBars` of loop audio PLUS `tailSeconds` of
 * FX tail (so the later loop-wrap has a tail to fold back over the head). The
 * returned buffer therefore covers `(lengthBars / cps) + tailSeconds` seconds.
 */
export interface RenderBackend {
  render(params: RenderParams): Promise<StereoBuffer>;
}
