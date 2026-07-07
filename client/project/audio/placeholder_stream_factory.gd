# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — PLACEHOLDER audio stream factory (ZoneMusicPlayer, #144).
#
# ⚠️  PLACEHOLDER ONLY — THIS IS NOT MUSIC.  ⚠️
# Per the owner ruling on #144, the ZoneMusicPlayer *system* is built at M0
# against synthesized placeholder tracks; the real audio direction (#143) is
# deferred. This factory procedurally generates a seamless looping sine tone per
# stem so the runtime, the vertical layer mix, and the crossfade/transition logic
# can be exercised end-to-end with zero audio assets committed to the repo.
#
# Each stem gets a distinct pitch (from the config's `placeholder_tone_hz`) so a
# listener can hear layers enter/leave, but no combination is remotely musical —
# that is intentional. When #143 lands, real `mus.*` stems resolve through the
# same ID hook path (PackEntry.resource) and this factory is unused in shipping.
class_name PlaceholderStreamFactory
extends RefCounted

const _SAMPLE_RATE := 44100
const _AMP := 0.20  # −14 dBFS-ish; well under a real −16 LUFS master, kept quiet.


# Build a seamless-looping PCM sine `AudioStreamWAV` at `hz`. The buffer length
# is rounded to a whole number of periods so the loop point has no seam (the
# property the post-encode seam lint, music SAD §4.4, guards for real assets).
static func make_tone(hz: float, seconds: float = 2.0) -> AudioStreamWAV:
	var period_samples := float(_SAMPLE_RATE) / maxf(hz, 1.0)
	var want := int(round(seconds * _SAMPLE_RATE))
	# Snap to a whole number of periods for a click-free loop.
	var periods := maxi(1, int(round(float(want) / period_samples)))
	var frames := int(round(periods * period_samples))

	var data := PackedByteArray()
	data.resize(frames * 2)  # 16-bit mono PCM
	for i in range(frames):
		var t := float(i) / float(_SAMPLE_RATE)
		var v := sin(TAU * hz * t) * _AMP
		var s := int(clampf(v, -1.0, 1.0) * 32767.0)
		data.encode_s16(i * 2, s)

	var stream := AudioStreamWAV.new()
	stream.format = AudioStreamWAV.FORMAT_16_BITS
	stream.stereo = false
	stream.mix_rate = _SAMPLE_RATE
	stream.data = data
	stream.loop_mode = AudioStreamWAV.LOOP_FORWARD
	stream.loop_begin = 0
	stream.loop_end = frames
	return stream


# Resolve one stem asset id to a placeholder stream. This is the M0 stand-in for
# the #148 ID hook path (asset id -> PackEntry.resource -> ResourceLoader): here
# the "resource" is synthesized from the config's tone hz instead of loaded.
static func resolve_stem(asset_id: String, tone_hz: float, seconds: float = 2.0) -> AudioStreamWAV:
	var stream := make_tone(tone_hz, seconds)
	# Tag the placeholder so nothing downstream mistakes it for a real asset.
	stream.resource_name = "PLACEHOLDER:%s@%.2fHz" % [asset_id, tone_hz]
	return stream
