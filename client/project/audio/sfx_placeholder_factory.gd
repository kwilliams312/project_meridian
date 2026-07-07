# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — PLACEHOLDER SFX one-shot factory (SFX trigger runtime, #148).
#
# ⚠️  PLACEHOLDER ONLY — THESE ARE NOT REAL SOUND EFFECTS.  ⚠️
# Per the owner ruling (placeholder audio is fine well into M4; build the SYSTEM),
# the SFX trigger runtime is built at M0 against synthesized one-shots; the real
# audio direction (#143) is deferred. This factory procedurally generates a short
# enveloped tone (fast attack, exponential decay, NON-looping) per sfx id so the
# event->id->one-shot trigger path and the bus routing can be exercised
# end-to-end with zero audio assets committed to the repo.
#
# Each sfx id gets a distinct pitch + length (from the config's placeholder_tone_hz
# / placeholder_ms) so a listener can tell a UI click from a footstep, but none of
# them is a real sound effect — that is intentional. When #143 lands, real `sfx.*`
# assets resolve through the same ID hook path (PackEntry.resource) and this
# factory is unused in shipping.
#
# The one-shot counterpart of placeholder_stream_factory.gd (#144), which makes
# seamless LOOPING tones for music stems; SFX one-shots must NOT loop.
class_name SfxPlaceholderFactory
extends RefCounted

const _SAMPLE_RATE := 44100
const _AMP := 0.25  # peak before the decay envelope; kept well under 0 dBFS.


# Build a short, non-looping enveloped PCM sine `AudioStreamWAV` at `hz`. A fast
# 5 ms attack avoids a click at onset; an exponential decay over the remainder
# gives a percussive one-shot that ends in silence (so no loop is needed).
static func make_oneshot(hz: float, ms: int) -> AudioStreamWAV:
	var seconds := maxf(0.01, float(ms) / 1000.0)
	var frames := int(round(seconds * _SAMPLE_RATE))
	var attack_frames := maxi(1, int(round(0.005 * _SAMPLE_RATE)))

	var decay_frames := maxi(1, frames - attack_frames)
	var data := PackedByteArray()
	data.resize(frames * 2)  # 16-bit mono PCM
	for i in range(frames):
		var t := float(i) / float(_SAMPLE_RATE)
		# Fast attack ramp, then exponential decay to silence.
		var env := 1.0
		if i < attack_frames:
			env = float(i) / float(attack_frames)
		else:
			env = exp(-5.0 * float(i - attack_frames) / float(decay_frames))
		var v := sin(TAU * hz * t) * _AMP * env
		var s := int(clampf(v, -1.0, 1.0) * 32767.0)
		data.encode_s16(i * 2, s)

	var stream := AudioStreamWAV.new()
	stream.format = AudioStreamWAV.FORMAT_16_BITS
	stream.stereo = false
	stream.mix_rate = _SAMPLE_RATE
	stream.data = data
	stream.loop_mode = AudioStreamWAV.LOOP_DISABLED  # one-shots never loop
	return stream


# Resolve one sfx asset id to a placeholder one-shot. This is the M0 stand-in for
# the #148 ID hook path (asset id -> PackEntry.resource -> ResourceLoader): here
# the "resource" is synthesized from the config's tone/length instead of loaded.
static func resolve_sfx(sfx_id: String, tone_hz: float, ms: int) -> AudioStreamWAV:
	var stream := make_oneshot(tone_hz, ms)
	# Tag the placeholder so nothing downstream mistakes it for a real asset.
	stream.resource_name = "PLACEHOLDER:%s@%.2fHz/%dms" % [sfx_id, tone_hz, ms]
	return stream
