<!-- SPDX-License-Identifier: Apache-2.0 -->
# `client/project/audio` — ZoneMusicPlayer v1 (AUD-02, issue #144)

The client-side adaptive-music runtime (music [SAD §2](../../../docs/sad/music-sad.md)).
Given a zone id it plays that zone's music **set**; on a state change it
**crossfades** the vertical layer mix (explore / tension / combat / silence).

> ⚠️ **PLACEHOLDER AUDIO.** Per the owner ruling on #144, the *system* is built at
> M0 against **synthesized placeholder tones** — `placeholder_stream_factory.gd`
> procedurally generates one seamless-looping sine per stem. **This is not music
> and does not claim to be.** The real audio direction (#143) is deferred; when it
> lands, real `mus.*` stems resolve through the same ID hook path and the factory
> is unused in shipping. No audio asset is committed to the repo.

## Files

| File | Role |
|------|------|
| `zone_music_player.gd` | `ZoneMusicPlayer` node — the audio graph (`AudioStreamSynchronized` stem stack), bar/beat-quantized crossfades, shadow bar clock, timing-seam telemetry. |
| `music_state_core.gd` | Engine-free (`RefCounted`) state machine + transition table (SAD §2.1) + crossfade scheduler + sample-domain bar clock. 1:1 mirror of the Python reference. |
| `zone_track_map.gd` | Engine-free zone → set → stem resolution from `zone_music_config.json`. |
| `placeholder_stream_factory.gd` | **PLACEHOLDER** procedural tone generator (stand-in for the #148 ID→resource hook). |
| `zone_music_config.json` | Zone→set→stem mapping + musical metadata. Data, not code (SAD §6.1). |
| `zone_music_player_verify.gd` | Headless runtime verify (`SceneTree`, PASS/FAIL, exit 0/1). |
| `music_timing_probe.gd` | TD-11 timing probe — drives a live player, emits framed JSON for the `GodotTimingSource` (#145 harness). |
| `sfx_player.gd` | **`SfxPlayer`** node (#148) — the SFX trigger runtime: event → sfx-id → one-shot via the ID hook, category → bus routing, pooled `AudioStreamPlayer`s. |
| `sfx_event_map.gd` | Engine-free event → sfx-id registry + category → bus/group routing from `sfx_config.json`. 1:1 mirror of the Python reference. |
| `sfx_placeholder_factory.gd` | **PLACEHOLDER** procedural one-shot generator (stand-in for the #148 ID→resource hook). One-shots never loop. |
| `sfx_config.json` | Event → sfx-id map + category → bus routing + ~10 placeholder one-shots. Data, not code (SAD §5). |
| `sfx_player_verify.gd` | Headless SFX runtime verify (`SceneTree`, PASS/FAIL, exit 0/1). |

## Design (music SAD §2.1–2.4)

- **Vertical layering.** One `AudioStreamSynchronized` per set; all stems play
  sample-locked, per-stem volume is the mix, **L1 (bed) never stops** — that is
  what makes transitions musical. `explore = L1+L2`, `tension = L1+L3`,
  `combat = L1+L3+L4`, `silence = rest`.
- **Transitions.** Combat *entry* is next-**beat** quantized (≤500 ms, feels
  immediate) with a combat-entry stinger; combat *exit* has 4 s hysteresis and a
  2-bar next-**bar** equal-power fade. Full table in `music_state_core.gd`
  (`rule_for`), matching SAD §2.1.
- **One pending transition** (last-writer-wins). It fires when the sample-domain
  shadow clock crosses the quantized boundary — the poll granularity is exactly
  the error TD-11 measures.

## Zone→track mapping & the ID hook path (#148)

Content references stable **asset IDs only** (`core:mus.*`), never file paths
(music PRD §7). `zone_music_config.json` maps `zone → set → [stems]`; the runtime
datastore (IF-5 pack manifest, `PackEntry.resource`) resolves each asset ID to a
`res://meridian/...` resource. At M0 that resolver is the placeholder factory. The
**same** JSON is loaded by the Python reference (`tools/zone_music`) so the two
agree by construction.

## The SFX trigger runtime & the ID hook path (#148)

The SFX counterpart of ZoneMusicPlayer. Music resolves *zone → set → stem*; SFX
resolves *event → sfx-id → one-shot*, through the **same** content-ID hook (music
[SAD §5](../../../docs/sad/music-sad.md)). Two id sources feed one hook:

- **Client-authored events** (footsteps by surface, UI clicks, the login sting)
  have no content object, so `sfx_config.json`'s `events` table maps a stable
  event key → `sfx.*` id.
- **Content-carried ids** (an ability's `audio_visual.cast_sfx` / `impact_sfx`,
  an NPC `sound_set`) already carry their `sfx.*` id in `/content` and resolve
  through the *identical* hook — no parallel path.

`SfxPlayer.play_event()` / `play_footstep()` / `play_sfx()` / `play_content_sfx()`
resolve the id, run it through the placeholder factory (M0), and route it to its
category's bus over the SAD §2.5 SFX/UI bus tree. The **same** `sfx_config.json`
is loaded by the Python reference (`tools/zone_sfx`) so the two agree by
construction.

> ⚠️ **PLACEHOLDER AUDIO.** `sfx_placeholder_factory.gd` synthesizes short
> enveloped one-shots (clicks/blips), **not** real sound effects. **M0 builds the
> routing/trigger SYSTEM, not sound content.** No audio asset is committed.

**Not in M0 scope** (deferred to M1, SAD §2.5 / §7): the voice manager
(concurrency-group caps, priority eviction, own-action guarantee, occlusion) and
spatial `AudioStreamPlayer3D` attenuation. The `cap` in the config is carried as
data for that work but is not enforced at M0.

## The TD-11 timing seam (#145 / #147)

`ZoneMusicPlayer` exposes the sample-domain seam the TD-11 harness measures
(music SAD §3.1): a **shadow bar clock** (`predicted_boundary`), per-stem
**gain-edge telemetry** (`drain_gain_edges`), per-stem **positions**
(`stem_positions`), and the last fired transition (`last_transition`).
`music_timing_probe.gd` drives a live player and prints the harness's
`TransitionEvent` / `DriftSample` records as framed JSON; the harness's
`GodotTimingSource` (`tools/td11_music_timing/probe.py`) shells out to it. #147
runs `--source godot` under the #111 bot fleet for the gate ruling.

## Running

```bash
# Engine-free logic (runs anywhere — no Godot, no audio device):
uv run pytest tests/test_zone_music.py tests/test_zone_sfx.py -q

# Headless runtime verify (needs a Godot 4.7 binary; Dummy audio driver, no device):
godot --headless --path client/project --script res://audio/zone_music_player_verify.gd
godot --headless --path client/project --script res://audio/sfx_player_verify.gd

# Real-audio TD-11 measurement through the seam (needs Godot 4.7):
GODOT_BIN=/path/to/godot PYTHONPATH=tools python3 -m td11_music_timing --source godot --out out/td11
```

**Audio confirmation needs a device** — the *sound* and the *feel* of the
crossfade are owner-confirmed; the headless checks prove the player instantiates,
loads the placeholder stack, and produces the correct schedule/state/timing.
